#include "marketdata_payload.h"
#include "shm_writer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include "TDFAPI.h"
#include "TDFAPIStruct.h"

namespace mdg {

#if defined(_WIN32)
static std::atomic<bool> g_stop(false);
#else
static volatile sig_atomic_t g_stop = 0;
#endif

#if defined(_WIN32)
static BOOL WINAPI ConsoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_stop.store(true, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}
#else
static void SignalHandler(int) { g_stop = 1; }
#endif

static bool StopRequested() {
#if defined(_WIN32)
  return g_stop.load(std::memory_order_relaxed);
#else
  return g_stop != 0;
#endif
}

static uint64_t NowMonotonicNs() {
#if defined(_WIN32)
  LARGE_INTEGER freq;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  const double seconds = static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
  return static_cast<uint64_t>(seconds * 1000000000.0);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

static std::string TimeToString(int nTime) {
  int hour = nTime / 10000000;
  int minute = (nTime / 100000) % 100;
  int second = (nTime / 1000) % 100;
  int ms = nTime % 1000;

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", hour, minute, second, ms);
  return std::string(buf);
}

static void SleepMs(uint32_t ms) {
#if defined(_WIN32)
  Sleep(ms);
#else
  usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

static inline void CpuRelax() {
#if defined(_MSC_VER)
  YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
  __asm__ __volatile__("pause" ::: "memory");
#else
  // no-op
#endif
}

static inline void LockSpin(volatile long* p) {
#if defined(_WIN32)
  while (InterlockedExchange(p, 1) != 0) {
    CpuRelax();
  }
#else
  while (__sync_lock_test_and_set(p, 1) != 0) {
    CpuRelax();
  }
#endif
}

static inline void UnlockSpin(volatile long* p) {
#if defined(_WIN32)
  InterlockedExchange(p, 0);
#else
  __sync_lock_release(p);
#endif
}

static bool FileExists(const std::string& path) {
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path.c_str());
  return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static void EnsureDir(const char* path) {
  if (!path || !*path) return;
#if defined(_WIN32)
  CreateDirectoryA(path, nullptr);
#else
  ::mkdir(path, 0777);
#endif
}

static bool ReadAllText(const std::string& path, std::string* out) {
  out->clear();
  std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
  if (!f.is_open()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static std::string DirName(const std::string& path) {
  if (path.empty()) return ".";
  size_t p = path.find_last_of("/\\");
  if (p == std::string::npos) return ".";
  if (p == 0) return path.substr(0, 1);
  return path.substr(0, p);
}

static std::string JoinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const char last = a[a.size() - 1];
  if (last == '/' || last == '\\') return a + b;
#if defined(_WIN32)
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

static std::string GetExePath() {
#if defined(_WIN32)
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  return std::string(buf, buf + n);
#else
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return std::string();
  buf[n] = '\0';
  return std::string(buf);
#endif
}

static std::string GetExeDir() { return DirName(GetExePath()); }

static std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

static bool EndsWithCaseInsensitive(const std::string& s, const char* suffix) {
  if (!suffix) return false;
  const size_t n = s.size();
  const size_t m = std::strlen(suffix);
  if (m == 0 || n < m) return false;
  for (size_t i = 0; i < m; ++i) {
    char a = s[n - m + i];
    char b = suffix[i];
    a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
    b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
    if (a != b) return false;
  }
  return true;
}

// Find the latest modified *.csv in a directory. Returns empty if none found.
static std::string FindLatestCsvInDir(const std::string& dir) {
  if (dir.empty()) return std::string();

#if defined(_WIN32)
  std::string pattern = dir;
  if (!pattern.empty()) {
    const char last = pattern[pattern.size() - 1];
    if (last != '\\' && last != '/') pattern += "\\";
  }
  pattern += "*.csv";

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return std::string();
  }

  FILETIME best = {0, 0};
  std::string best_name;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
      best = fd.ftLastWriteTime;
      best_name = fd.cFileName;
    }
  } while (FindNextFileA(h, &fd) != 0);

  FindClose(h);
  if (best_name.empty()) return std::string();
  return JoinPath(dir, best_name);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return std::string();

  time_t best_mtime = 0;
  std::string best_name;
  for (;;) {
    struct dirent* ent = readdir(d);
    if (!ent) break;
    const char* name = ent->d_name;
    if (!name) continue;
    std::string fn(name);
    if (!EndsWithCaseInsensitive(fn, ".csv")) continue;
    const std::string full = JoinPath(dir, fn);
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    if (st.st_mtime >= best_mtime) {
      best_mtime = st.st_mtime;
      best_name = fn;
    }
  }

  closedir(d);
  if (best_name.empty()) return std::string();
  return JoinPath(dir, best_name);
#endif
}

static bool ParseCsvSymbols(const std::string& csv_path, std::vector<std::string>* out_wind_codes) {
  out_wind_codes->clear();
  std::ifstream file(csv_path.c_str());
  if (!file.is_open()) {
    std::cerr << "[md_gate] failed to open csv: " << csv_path << std::endl;
    return false;
  }

  std::string line;
  bool header = true;
  while (std::getline(file, line)) {
    if (header) {
      header = false;
      continue;
    }
    if (line.empty()) continue;

    std::stringstream ss(line);
    std::string token;
    int col = 0;
    std::string symbol6;
    while (std::getline(ss, token, ',')) {
      ++col;
      if (col == 3) {
        symbol6 = Trim(token);
        break;
      }
    }
    if (symbol6.size() != 6) continue;

    std::string wind = symbol6;
    if (!wind.empty() && wind[0] == '6') {
      wind += ".SH";
    } else {
      wind += ".SZ";
    }
    out_wind_codes->push_back(wind);
  }
  return true;
}

static std::string JoinSubscriptions(const std::vector<std::string>& wind_codes) {
  std::string out;
  for (size_t i = 0; i < wind_codes.size(); ++i) {
    if (i) out.push_back(';');
    out += wind_codes[i];
  }
  return out;
}

static std::string ExtractNumericCode(const std::string& wind_code) {
  const size_t pos = wind_code.find('.');
  if (pos == std::string::npos) return wind_code;
  return wind_code.substr(0, pos);
}

static bool ParseWindCodeKey(const char* wind_code, uint32_t* out_key, char out_wind16[16]) {
  if (!wind_code || !out_key || !out_wind16) return false;

  // Bounded parse: accept "600000.SH" / "000001.SZ" (min 9 chars).
  // We only accept exactly 6 digits, then '.', then 'S' + {'H'|'Z'}.
  char digits[6];
  for (int i = 0; i < 6; ++i) {
    const char c = wind_code[i];
    if (c < '0' || c > '9') return false;
    digits[i] = c;
  }
  if (wind_code[6] != '.') return false;
  char m0 = wind_code[7];
  char m1 = wind_code[8];
  m0 = static_cast<char>(std::toupper(static_cast<unsigned char>(m0)));
  m1 = static_cast<char>(std::toupper(static_cast<unsigned char>(m1)));
  if (m0 != 'S') return false;

  uint32_t market = 0;
  if (m1 == 'H') market = 1;
  else if (m1 == 'Z') market = 0;
  else return false;

  uint32_t code = 0;
  for (int i = 0; i < 6; ++i) {
    code = code * 10u + static_cast<uint32_t>(digits[i] - '0');
  }

  *out_key = market * 1000000u + code;

  std::memset(out_wind16, 0, 16);
  out_wind16[0] = digits[0];
  out_wind16[1] = digits[1];
  out_wind16[2] = digits[2];
  out_wind16[3] = digits[3];
  out_wind16[4] = digits[4];
  out_wind16[5] = digits[5];
  out_wind16[6] = '.';
  out_wind16[7] = 'S';
  out_wind16[8] = (market == 1) ? 'H' : 'Z';
  out_wind16[9] = '\0';
  return true;
}

static bool ContainsSTToken(const char* raw, size_t len) {
  if (!raw) return false;
  std::string buffer;
  buffer.reserve(len);
  for (size_t i = 0; i < len && raw[i] != '\0'; ++i) {
    unsigned char ch = static_cast<unsigned char>(raw[i]);
    buffer.push_back(static_cast<char>(std::toupper(ch)));
  }
  return buffer.find("ST") != std::string::npos;
}

static bool IsStSecurity(const TDF_MARKET_DATA& data) {
  if (ContainsSTToken(data.chPrefix, sizeof(data.chPrefix))) return true;
  if (data.pCodeInfo && ContainsSTToken(data.pCodeInfo->chName, sizeof(data.pCodeInfo->chName))) return true;
  return false;
}

static int64_t RoundPriceToX10000(double price) {
  // Round to 0.01, then scale to x10000.
  if (!(price >= 0.0)) return 0;
  const double rounded = std::floor(price * 100.0 + 0.5) / 100.0;
  return static_cast<int64_t>(rounded * 10000.0 + 0.5);
}

static double DeduceLimitRatioFast(const char wind_code16[16], const TDF_MARKET_DATA& data) {
  if (wind_code16 && wind_code16[0] && wind_code16[1]) {
    if ((wind_code16[0] == '3' && wind_code16[1] == '0') ||
        (wind_code16[0] == '6' && wind_code16[1] == '8')) {
      return 0.20; // 创业板/科创板
    }
  }
  if (IsStSecurity(data)) return 0.05;
  return 0.10;
}

static void BuildLimitFallback(int64_t pre_close_x10000, double ratio, int64_t* out_up, int64_t* out_down) {
  if (!out_up || !out_down) return;
  *out_up = 0;
  *out_down = 0;
  if (pre_close_x10000 <= 0) return;
  if (!(ratio > 0.0)) return;

  const double pre_close = static_cast<double>(pre_close_x10000) / 10000.0;
  const double up = pre_close * (1.0 + ratio);
  const double down = pre_close * (1.0 - ratio);
  *out_up = RoundPriceToX10000(up);
  *out_down = RoundPriceToX10000((std::max)(0.0, down));
}

static bool ExtractJsonObject(const std::string& s, const std::string& key, std::string* out_obj) {
  out_obj->clear();
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return false;
  p = s.find('{', p);
  if (p == std::string::npos) return false;
  size_t i = p;
  int depth = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '{') ++depth;
    else if (c == '}') {
      --depth;
      if (depth == 0) {
        *out_obj = s.substr(p, i - p + 1);
        return true;
      }
    }
  }
  return false;
}

static bool JsonGetString(const std::string& obj, const std::string& key, std::string* out) {
  const std::string pat = "\"" + key + "\"";
  size_t p = obj.find(pat);
  if (p == std::string::npos) return false;
  p = obj.find(':', p);
  if (p == std::string::npos) return false;
  ++p;
  while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p]))) ++p;
  if (p >= obj.size() || obj[p] != '"') return false;
  ++p;
  size_t e = obj.find('"', p);
  if (e == std::string::npos) return false;
  *out = obj.substr(p, e - p);
  return true;
}

static bool JsonGetInt(const std::string& obj, const std::string& key, int* out) {
  const std::string pat = "\"" + key + "\"";
  size_t p = obj.find(pat);
  if (p == std::string::npos) return false;
  p = obj.find(':', p);
  if (p == std::string::npos) return false;
  ++p;
  while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p]))) ++p;
  size_t e = p;
  if (e < obj.size() && (obj[e] == '-' || obj[e] == '+')) ++e;
  while (e < obj.size() && std::isdigit(static_cast<unsigned char>(obj[e]))) ++e;
  if (e == p) return false;
  *out = std::atoi(obj.substr(p, e - p).c_str());
  return true;
}

struct Options {
  std::string config_path;  // if empty, auto-detect
  std::string host = "58.210.86.54";
  int port = 10001;
  std::string user = "test";
  std::string password = "test";
  std::string csv_path = "./data/subscribe_all.csv";
  std::string shm_name =
#if defined(_WIN32)
      "md_gate_shm";
#else
      "/md_gate_shm";
#endif
  uint32_t symbol_count = kMaxSymbols;
  uint32_t type_flags = 0;      // DATA_TYPE_NONE (snapshot only). For transaction/order/orderqueue use bit-or.
  uint32_t heartbeat_ms = 500;
  uint32_t print_limit = 20;    // print first N received snapshots (0=disable)
  bool unlink_on_exit = false;
  uint32_t mock_interval_ms = 1000;  // 模拟行情间隔（毫秒）
  bool replay_mode = false;     // 历史回放模式：nTime=0xFFFFFFFF，用于测试服务器回放历史行情
};

static void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "  --config <config.json> (optional, default auto-detect config.json)\n"
      << "  --host <ip>\n"
      << "  --port <port>\n"
      << "  --user <user>\n"
      << "  --password <pwd>\n"
      << "  --csv <config.csv>\n"
      << "  --shm <name>          (linux must start with '/')\n"
      << "  --symbol-count <n>    (<=3000)\n"
      << "  --type-flags <n>      (0=snapshot only; 2=TRANSACTION; 4=ORDER; 8=ORDERQUEUE; combine with |)\n"
      << "  --heartbeat-ms <ms>\n"
      << "  --print <n>           (print first n snapshots; 0=disable)\n"
      << "  --unlink-on-exit\n"
      << "  --mock                (mock mode: generate fake market data without TDF connection)\n"
      << "  --mock-interval <ms>  (mock data interval in milliseconds, default 1000)\n"
      << "  --replay              (historical replay mode: nTime=0xFFFFFFFF for test server)\n";
}

static bool ApplyConfigJson(const std::string& path, Options* opt) {
  std::string txt;
  if (!ReadAllText(path, &txt)) return false;

  std::string market_obj;
  if (ExtractJsonObject(txt, "market", &market_obj)) {
    std::string v;
    int iv = 0;
    if (JsonGetString(market_obj, "host", &v) && !v.empty()) opt->host = v;
    if (JsonGetInt(market_obj, "port", &iv) && iv > 0) opt->port = iv;
    if (JsonGetString(market_obj, "user", &v) && !v.empty()) opt->user = v;
    if (JsonGetString(market_obj, "password", &v) && !v.empty()) opt->password = v;
    // Optional: non-snapshot subscription types (TRANSACTION/ORDER/ORDERQUEUE).
    // 0 = snapshot only, 2=TRANSACTION, 4=ORDER, 8=ORDERQUEUE, combine with '|'.
    if (JsonGetInt(market_obj, "type_flags", &iv) && iv >= 0) opt->type_flags = static_cast<uint32_t>(iv);
    if (JsonGetInt(market_obj, "nTypeFlags", &iv) && iv >= 0) opt->type_flags = static_cast<uint32_t>(iv);
  }

  std::string strategy_obj;
  if (ExtractJsonObject(txt, "strategy", &strategy_obj)) {
    std::string v;
    if (JsonGetString(strategy_obj, "csv_path", &v) && !v.empty()) {
      opt->csv_path = v;
    }
  }

  return true;
}

static std::string AutoDetectConfigPath() {
  const std::string exe_dir = GetExeDir();
  if (exe_dir.empty()) return std::string();

  // Search (up to 5 levels):
  // - <dir>/config.json
  // - <dir>/gate_result/config.json
  // - <dir>/result/config.json
  std::string cur = exe_dir;
  for (int up = 0; up <= 5; ++up) {
    const std::string c0 = JoinPath(cur, "config.json");
    if (FileExists(c0)) return c0;

    const std::string c1 = JoinPath(cur, "gate_result/config.json");
    if (FileExists(c1)) return c1;

    const std::string c2 = JoinPath(cur, "result/config.json");
    if (FileExists(c2)) return c2;

    // move one level up
    const std::string parent = DirName(cur);
    if (parent == cur) break;
    cur = parent;
  }
  return std::string();
}

static bool ParseArgs(int argc, char** argv, Options* opt) {
  // 1) First pass: find --config
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "[md_gate] missing value for --config" << std::endl;
        return false;
      }
      opt->config_path = argv[i + 1];
      break;
    }
  }

  // 2) Load config.json (default auto-detect) before applying CLI overrides.
  std::string cfg = opt->config_path;
  if (cfg.empty()) {
    // Prefer working-directory config.json (e.g. bin/ deployment folder).
    if (FileExists("config.json")) {
      cfg = "config.json";
    } else {
      cfg = AutoDetectConfigPath();
    }
  }
  if (!cfg.empty() && FileExists(cfg)) {
    if (ApplyConfigJson(cfg, opt)) {
      opt->config_path = cfg;
      std::cout << "[md_gate] config=" << cfg << std::endl;
    } else {
      std::cerr << "[md_gate] failed to parse config: " << cfg << std::endl;
    }
  }

  // 3) Second pass: apply all CLI overrides.
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    auto need = [&](const char* key) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "[md_gate] missing value for " << key << std::endl;
        return nullptr;
      }
      return argv[++i];
    };

    if (a == "--config") {
      const char* v = need("--config");
      if (!v) return false;
      opt->config_path = v;
    } else if (a == "--host") {
      const char* v = need("--host");
      if (!v) return false;
      opt->host = v;
    } else if (a == "--port") {
      const char* v = need("--port");
      if (!v) return false;
      opt->port = std::atoi(v);
    } else if (a == "--user") {
      const char* v = need("--user");
      if (!v) return false;
      opt->user = v;
    } else if (a == "--password") {
      const char* v = need("--password");
      if (!v) return false;
      opt->password = v;
    } else if (a == "--csv") {
      const char* v = need("--csv");
      if (!v) return false;
      opt->csv_path = v;
    } else if (a == "--shm") {
      const char* v = need("--shm");
      if (!v) return false;
      opt->shm_name = v;
    } else if (a == "--symbol-count") {
      const char* v = need("--symbol-count");
      if (!v) return false;
      opt->symbol_count = static_cast<uint32_t>(std::atoi(v));
    } else if (a == "--type-flags") {
      const char* v = need("--type-flags");
      if (!v) return false;
      opt->type_flags = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--heartbeat-ms") {
      const char* v = need("--heartbeat-ms");
      if (!v) return false;
      opt->heartbeat_ms = static_cast<uint32_t>(std::atoi(v));
    } else if (a == "--print") {
      const char* v = need("--print");
      if (!v) return false;
      opt->print_limit = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--unlink-on-exit") {
      opt->unlink_on_exit = true;
    } else if (a == "--mock-interval") {
      const char* v = need("--mock-interval");
      if (!v) return false;
      opt->mock_interval_ms = static_cast<uint32_t>(std::atoi(v));
    } else if (a == "--replay") {
      opt->replay_mode = true;
    } else if (a == "-h" || a == "--help") {
      PrintUsage(argv[0]);
      return false;
    } else {
      std::cerr << "[md_gate] unknown arg: " << a << std::endl;
      PrintUsage(argv[0]);
      return false;
    }
  }
  return true;
}

class MdGateApp {
public:
  explicit MdGateApp(const Options& opt) : opt_(opt), connected_(false) {}

  bool Init() {
    if (opt_.symbol_count == 0 || opt_.symbol_count > kMaxSymbols) {
      std::cerr << "[md_gate] invalid --symbol-count: " << opt_.symbol_count << std::endl;
      return false;
    }

    // CSV default fallback:
    // 1) If ./data/subscribe_all.csv is missing, try latest *.csv under ./data.
    // 2) If still missing, fallback to ./config.csv (legacy behavior).
    if (!FileExists(opt_.csv_path) && opt_.csv_path == "./data/subscribe_all.csv") {
      const std::string latest = FindLatestCsvInDir("./data");
      if (!latest.empty()) {
        std::cout << "[md_gate] csv not found, fallback to latest in ./data: " << latest << std::endl;
        opt_.csv_path = latest;
      } else if (FileExists("./config.csv")) {
        std::cout << "[md_gate] csv not found, fallback to ./config.csv" << std::endl;
        opt_.csv_path = "./config.csv";
      }
    }
    if (!ParseCsvSymbols(opt_.csv_path, &wind_codes_)) return false;
    if (wind_codes_.size() > opt_.symbol_count) {
      std::cerr << "[md_gate] csv symbols=" << wind_codes_.size()
                << " exceeds symbol_count=" << opt_.symbol_count << std::endl;
      return false;
    }

    codekey2id_.clear();
    codekey2id_.reserve(wind_codes_.size() * 2 + 1);
    for (size_t i = 0; i < wind_codes_.size(); ++i) {
      uint32_t key = 0;
      char canon[16];
      if (!ParseWindCodeKey(wind_codes_[i].c_str(), &key, canon)) {
        continue;
      }
      codekey2id_[key] = static_cast<uint32_t>(i);
    }

    // Per-symbol writer locks (0=unlocked, 1=locked). Used only inside md_gate process.
    entry_locks_.assign(opt_.symbol_count, 0L);

    subscriptions_ = JoinSubscriptions(wind_codes_);
    std::cout << "[md_gate] csv=" << opt_.csv_path << " symbols=" << wind_codes_.size() << std::endl;
    std::cout << "[md_gate] shm=" << opt_.shm_name << " symbol_count=" << opt_.symbol_count << std::endl;

    if (!writer_.Create(opt_.shm_name.c_str(), opt_.symbol_count)) {
      std::cerr << "[md_gate] shm create failed errno=" << writer_.last_errno() << std::endl;
      return false;
    }

    // Publish id -> wind_code directory into SHM (symbol_dir).
    // This allows trade processes to use their own smaller CSV subsets while still locating the correct slot.
    for (size_t i = 0; i < wind_codes_.size(); ++i) {
      writer_.WriteSymbolDirEntry(static_cast<uint32_t>(i), wind_codes_[i].c_str());
    }

    // Mark as (re)connecting until login success.
    writer_.SetMdStatus(2);
    writer_.SetLastErr(0);
    return true;
  }

  bool ConnectTdf() {
    if (connected_) return true;

    // TDF setup
    EnsureDir("./log");
    TDF_SetLogPath("./log");
    TDF_SetEnv(TDF_ENVIRON_HEART_BEAT_INTERVAL, 10);
    TDF_SetEnv(TDF_ENVIRON_MISSED_BEART_COUNT, 3);
    TDF_SetEnv(TDF_ENVIRON_OPEN_TIME_OUT, 30);

    TDF_OPEN_SETTING_EXT settings;
    std::memset(&settings, 0, sizeof(settings));

    strncpy_s(settings.siServer[0].szIp, opt_.host.c_str(), sizeof(settings.siServer[0].szIp) - 1);
    std::snprintf(settings.siServer[0].szPort, sizeof(settings.siServer[0].szPort), "%d", opt_.port);
    strncpy_s(settings.siServer[0].szUser, opt_.user.c_str(), sizeof(settings.siServer[0].szUser) - 1);
    strncpy_s(settings.siServer[0].szPwd, opt_.password.c_str(), sizeof(settings.siServer[0].szPwd) - 1);
    settings.nServerNum = 1;

    settings.pfnMsgHandler = &MdGateApp::OnDataReceived;
    settings.pfnSysMsgNotify = &MdGateApp::OnSystemMessage;
    // nTime: 0 = realtime, 0xFFFFFFFF = replay from beginning (historical)
    settings.nTime = opt_.replay_mode ? 0xFFFFFFFFu : 0;
    if (opt_.replay_mode) {
      std::cout << "[md_gate] replay mode enabled (nTime=0xFFFFFFFF)" << std::endl;
    }
    settings.szMarkets = "SZ-2-0;SH-2-0";
    settings.szSubScriptions = subscriptions_.c_str();
    // nTypeFlags only controls non-snapshot types (transaction/order/orderqueue).
    // Snapshot market data is always delivered; keep default 0 unless explicitly required.
    settings.nTypeFlags = opt_.type_flags;

    // Enable callbacks BEFORE TDF_OpenExt so connect/login messages are received.
    // Note: tdf_ is still nullptr at this point, so callbacks will check running_ and g_app_ first.
    running_.store(true, std::memory_order_release);
    g_app_.store(this, std::memory_order_release);

    TDF_ERR err = TDF_ERR_SUCCESS;
    THANDLE opened = TDF_OpenExt(&settings, &err);
    if (err != TDF_ERR_SUCCESS) {
      int retry = 0;
      while (err == TDF_ERR_NETWORK_ERROR && retry < 3 && !StopRequested()) {
        ++retry;
        std::cerr << "[md_gate] TDF_OpenExt network error, retry " << retry << "/3" << std::endl;
        SleepMs(3000);
        opened = TDF_OpenExt(&settings, &err);
      }
    }

    if (err != TDF_ERR_SUCCESS || !opened) {
      std::cerr << "[md_gate] TDF_OpenExt failed err=" << static_cast<int>(err) << std::endl;
      writer_.SetLastErr(static_cast<uint32_t>(err));
      writer_.SetMdStatus(1); // DISCONNECTED
      running_.store(false, std::memory_order_release);
      g_app_.store(nullptr, std::memory_order_release);
      return false;
    }

    tdf_.store(opened, std::memory_order_release);

    connected_ = true;
    writer_.SetMdStatus(2); // RECONNECTING until login result arrives
    return true;
  }

  void Run() {
    // Heartbeat loop
    while (!StopRequested()) {
      const uint64_t now = NowMonotonicNs();
      writer_.UpdateHeartbeat(now);
      SleepMs(opt_.heartbeat_ms);
    }
  }

  void Shutdown() {
    running_.store(false, std::memory_order_release);
    g_app_.store(nullptr, std::memory_order_release);

    THANDLE h = tdf_.exchange(nullptr, std::memory_order_acq_rel);
    if (h) {
      TDF_Close(h);
    }

    // Ensure no callbacks are still running before unmapping SHM.
    for (int i = 0; i < 5000; ++i) {
      if (in_callback_.load(std::memory_order_acquire) == 0) break;
      SleepMs(1);
    }
    connected_ = false;

    if (opt_.unlink_on_exit) {
      writer_.Unlink(opt_.shm_name.c_str());
    }
    writer_.Close();
  }

private:
  static void OnDataReceived(THANDLE hTdf, TDF_MSG* pMsgHead) {
    MdGateApp* self = g_app_.load(std::memory_order_acquire);
    if (!self) return;
    if (!self->running_.load(std::memory_order_acquire)) return;
    // Allow callbacks during TDF_OpenExt when tdf_ is still nullptr.
    // Market data may arrive before TDF_OpenExt returns.
    THANDLE expected = self->tdf_.load(std::memory_order_acquire);
    if (expected && hTdf != expected) return;
    self->HandleData(pMsgHead);
  }

  static void OnSystemMessage(THANDLE hTdf, TDF_MSG* pSysMsg) {
    MdGateApp* self = g_app_.load(std::memory_order_acquire);
    if (!self) return;
    if (!self->running_.load(std::memory_order_acquire)) return;
    // For system messages (connect/login/codetable), allow callbacks during TDF_OpenExt
    // when tdf_ is still nullptr. Only reject if tdf_ is set and doesn't match.
    THANDLE expected = self->tdf_.load(std::memory_order_acquire);
    if (expected && hTdf != expected) return;
    self->HandleSystem(pSysMsg);
  }

  void HandleData(TDF_MSG* msg) {
    if (!msg || !msg->pData) return;
    if (msg->nDataType != MSG_DATA_MARKET) return;

    in_callback_.fetch_add(1, std::memory_order_acq_rel);
    struct Guard {
      std::atomic<uint32_t>* c;
      explicit Guard(std::atomic<uint32_t>* x) : c(x) {}
      ~Guard() { c->fetch_sub(1, std::memory_order_acq_rel); }
    } guard(&in_callback_);

    if (!msg->pAppHead) return;
    const int item_count = msg->pAppHead->nItemCount;
    const int item_size = msg->pAppHead->nItemSize;
    const int head_size = msg->pAppHead->nHeadSize;
    if (item_count <= 0) return;
    if (item_count > 100000) return;
    if (item_size != static_cast<int>(sizeof(TDF_MARKET_DATA))) return;
    if (head_size <= 0 || head_size > 1024) return;

    // TDFAPIStruct.h says nDataLen "includes TDF_APP_HEAD length", but some SDK builds / platforms may
    // report nDataLen as only payload bytes. Accept either to avoid dropping valid snapshots.
    const uint64_t payload_need =
        static_cast<uint64_t>(static_cast<uint32_t>(item_count)) * static_cast<uint64_t>(item_size);
    const uint64_t with_head_need = static_cast<uint64_t>(head_size) + payload_need;
    if (msg->nDataLen > 0) {
      const uint64_t got = static_cast<uint64_t>(msg->nDataLen);
      if (got < payload_need && got < with_head_need) return;
    }

    const TDF_MARKET_DATA* m = reinterpret_cast<const TDF_MARKET_DATA*>(msg->pData);
    const uint64_t now_ns = NowMonotonicNs();

    int matched = 0;
    for (int i = 0; i < item_count; ++i) {
      uint32_t key = 0;
      char wind16[16];
      if (!ParseWindCodeKey(m[i].szWindCode, &key, wind16)) continue;
      auto it = codekey2id_.find(key);
      if (it == codekey2id_.end()) continue;
      const uint32_t symbol_id = it->second;
      if (!writer_.header() || symbol_id >= writer_.header()->symbol_count) continue;

      ++matched;
      LockSpin(&entry_locks_[symbol_id]);

      MarketDataPayloadV1 payload;
      ZeroMarketDataPayload(&payload);
      payload.payload_version = 1;
      payload.flags = 1;
      payload.action_day = m[i].nActionDay;
      payload.trading_day = m[i].nTradingDay;
      payload.time_hhmmssmmm = m[i].nTime;
      payload.status = m[i].nStatus;

      payload.pre_close_x10000 = m[i].nPreClose;
      payload.open_x10000 = m[i].nOpen;
      payload.high_x10000 = m[i].nHigh;
      payload.low_x10000 = m[i].nLow;
      payload.last_x10000 = m[i].nMatch;

      payload.high_limit_x10000 = m[i].nHighLimited;
      payload.low_limit_x10000 = m[i].nLowLimited;
      if (payload.high_limit_x10000 <= 0 || payload.low_limit_x10000 <= 0) {
        const double ratio = DeduceLimitRatioFast(wind16, m[i]);
        int64_t up = 0, down = 0;
        BuildLimitFallback(payload.pre_close_x10000, ratio, &up, &down);
        if (payload.high_limit_x10000 <= 0) payload.high_limit_x10000 = up;
        if (payload.low_limit_x10000 <= 0) payload.low_limit_x10000 = down;
      }

      payload.volume = m[i].iVolume;
      payload.turnover = m[i].iTurnover;
      for (int k = 0; k < 5; ++k) {
        payload.bid_price_x10000[k] = m[i].nBidPrice[k];
        payload.bid_vol[k] = m[i].nBidVol[k];
        payload.ask_price_x10000[k] = m[i].nAskPrice[k];
        payload.ask_vol[k] = m[i].nAskVol[k];
      }

      std::memset(payload.wind_code, 0, sizeof(payload.wind_code));
      strncpy_s(payload.wind_code, wind16, sizeof(payload.wind_code) - 1);
      std::memset(payload.prefix, 0, sizeof(payload.prefix));
      std::memcpy(payload.prefix, m[i].chPrefix,
                  (std::min)(sizeof(payload.prefix), sizeof(m[i].chPrefix)));
      payload.recv_ns = now_ns;

      MarketData320 md;
      std::memset(md.bytes, 0, sizeof(md.bytes));
      std::memcpy(md.bytes, &payload, sizeof(payload));

      writer_.UpdateSnapshot(symbol_id, md, now_ns);
      store_u64_release(&writer_.header()->last_md_ns, now_ns);

      UnlockSpin(&entry_locks_[symbol_id]);

      // Print first N snapshots for testing/verification.
      if (opt_.print_limit != 0) {
        const uint32_t idx = printed_.fetch_add(1, std::memory_order_relaxed);
        if (idx < opt_.print_limit) {
          const double pre_close = static_cast<double>(payload.pre_close_x10000) / 10000.0;
          const double open = static_cast<double>(payload.open_x10000) / 10000.0;
          const double last = static_cast<double>(payload.last_x10000) / 10000.0;
          const double high_lim = static_cast<double>(payload.high_limit_x10000) / 10000.0;
          const double low_lim = static_cast<double>(payload.low_limit_x10000) / 10000.0;
          const double open_ratio = (pre_close > 0.0) ? (open / pre_close) : 0.0;
          const double turnover_wan = static_cast<double>(payload.turnover) / 10000.0;

          std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
          std::cout << "[md_gate] " << payload.wind_code << " " << TimeToString(payload.time_hhmmssmmm)
                    << " trading=" << payload.trading_day << " action=" << payload.action_day << std::endl;
          std::cout << "  pre_close=" << pre_close
                    << " open=" << open << " (ratio=" << open_ratio << ")"
                    << " last=" << last << std::endl;
          std::cout << "  high_lim=" << high_lim << " low_lim=" << low_lim << std::endl;
          std::cout << "  bid1=" << (static_cast<double>(payload.bid_price_x10000[0]) / 10000.0)
                    << " (" << payload.bid_vol[0] << ")"
                    << " | ask1=" << (static_cast<double>(payload.ask_price_x10000[0]) / 10000.0)
                    << " (" << payload.ask_vol[0] << ")" << std::endl;
          std::cout << "  volume=" << payload.volume
                    << " turnover=" << payload.turnover
                    << " (" << turnover_wan << "万)" << std::endl;

          if (idx + 1 == opt_.print_limit) {
            std::cout << "[md_gate] print limit reached (" << opt_.print_limit << "), continue writing SHM silently"
                      << std::endl;
          }
        }
      }
    }

    // If we receive market messages but cannot match any subscribed symbol, print one hint line.
    if (matched == 0 && opt_.print_limit != 0) {
      bool expected = false;
      if (warned_no_match_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::cout << "[md_gate] recv MSG_DATA_MARKET but no symbol matched; sample wind_code=" << m[0].szWindCode
                  << " item_count=" << item_count << " item_size=" << item_size << " head_size=" << head_size
                  << " nDataLen=" << msg->nDataLen << std::endl;
      }
    }
  }

  void HandleSystem(TDF_MSG* sys) {
    if (!sys) return;
    in_callback_.fetch_add(1, std::memory_order_acq_rel);
    struct Guard {
      std::atomic<uint32_t>* c;
      explicit Guard(std::atomic<uint32_t>* x) : c(x) {}
      ~Guard() { c->fetch_sub(1, std::memory_order_acq_rel); }
    } guard(&in_callback_);

    switch (sys->nDataType) {
      case MSG_SYS_DISCONNECT_NETWORK:
        std::cerr << "[md_gate] TDF disconnect" << std::endl;
        writer_.SetMdStatus(1);
        writer_.SetLastErr(1);
        break;
      case MSG_SYS_CONNECT_RESULT: {
        TDF_CONNECT_RESULT* r = reinterpret_cast<TDF_CONNECT_RESULT*>(sys->pData);
        if (r && r->nConnResult) {
          std::cout << "[md_gate] TDF connect ok " << r->szIp << ":" << r->szPort << std::endl;
          writer_.SetLastErr(0);
        } else {
          std::cerr << "[md_gate] TDF connect failed" << std::endl;
          writer_.SetMdStatus(1);
          writer_.SetLastErr(2);
        }
        break;
      }
      case MSG_SYS_LOGIN_RESULT: {
        TDF_LOGIN_RESULT* r = reinterpret_cast<TDF_LOGIN_RESULT*>(sys->pData);
        if (r && r->nLoginResult) {
          std::cout << "[md_gate] TDF login ok" << std::endl;
          writer_.SetMdStatus(0);
          writer_.SetLastErr(0);
        } else {
          std::cerr << "[md_gate] TDF login failed" << std::endl;
          writer_.SetMdStatus(1);
          writer_.SetLastErr(3);
        }
        break;
      }
      case MSG_SYS_CODETABLE_RESULT:
        std::cout << "[md_gate] TDF codetable ready" << std::endl;
        break;
      default:
        break;
    }
  }

private:
  Options opt_;
  ShmWriter writer_;
  std::atomic<THANDLE> tdf_{nullptr};
  bool connected_;

  std::vector<std::string> wind_codes_;
  std::unordered_map<uint32_t, uint32_t> codekey2id_;
  std::string subscriptions_;

  std::vector<long> entry_locks_;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> in_callback_{0};
  std::atomic<uint32_t> printed_{0};
  std::atomic<bool> warned_no_match_{false};

  static std::atomic<MdGateApp*> g_app_;
};

std::atomic<MdGateApp*> MdGateApp::g_app_(nullptr);

} // namespace mdg

int main(int argc, char** argv) {
  mdg::Options opt;
  if (!mdg::ParseArgs(argc, argv, &opt)) {
    return 2;
  }

#if defined(_WIN32)
  SetConsoleCtrlHandler(&mdg::ConsoleHandler, TRUE);
#else
  signal(SIGINT, &mdg::SignalHandler);
  signal(SIGTERM, &mdg::SignalHandler);
#endif

  mdg::MdGateApp app(opt);
  if (!app.Init()) return 1;
  if (!app.ConnectTdf()) {
    // Still run heartbeat so trade_app can detect gateway is alive but disconnected.
    std::cerr << "[md_gate] running heartbeat in disconnected state" << std::endl;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
