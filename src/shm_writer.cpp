#include "shm_writer.h"

#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace mdg {

namespace {

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

static uint32_t GetPid() {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentProcessId());
#else
  return static_cast<uint32_t>(getpid());
#endif
}

static uint32_t GetUid() {
#if defined(_WIN32)
  return 0;
#else
  return static_cast<uint32_t>(getuid());
#endif
}

static void SetLastErrno(int* out, int err) {
  if (out) *out = err;
}

} // namespace

ShmWriter::ShmWriter()
    : base_(nullptr),
      bytes_(0),
      header_(nullptr),
      symbol_dir_(nullptr),
      entries_(nullptr),
      create_symbol_count_(0),
#if defined(_WIN32)
      fd_(nullptr),
#else
      fd_(-1),
#endif
      last_errno_(0) {}

ShmWriter::~ShmWriter() { Close(); }

bool ShmWriter::Create(const char* shm_name, uint32_t symbol_count) {
  Close();
  last_errno_ = 0;

  if (!shm_name || !*shm_name) {
    last_errno_ = EINVAL;
    return false;
  }
  if (symbol_count == 0 || symbol_count > kMaxSymbols) {
    last_errno_ = EINVAL;
    return false;
  }

  create_symbol_count_ = symbol_count;
  const size_t header_bytes = align_up(sizeof(ShmHeader), kCacheLineBytes);
  const size_t symbol_dir_bytes = align_up(static_cast<size_t>(symbol_count) * static_cast<size_t>(kSymbolDirEntryBytes),
                                           kCacheLineBytes);
  const size_t snapshot_bytes = static_cast<size_t>(symbol_count) * sizeof(SnapshotEntry);
  const size_t total_bytes = header_bytes + symbol_dir_bytes + snapshot_bytes;

#if defined(_WIN32)
  HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               static_cast<DWORD>((total_bytes >> 32) & 0xffffffffu),
                               static_cast<DWORD>(total_bytes & 0xffffffffu), shm_name);
  if (!h) {
    last_errno_ = static_cast<int>(GetLastError());
    return false;
  }
  fd_ = h;
  return MapAndBind_(0, total_bytes, true);
#else
  int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
  if (fd < 0) {
    last_errno_ = errno;
    return false;
  }
  if (ftruncate(fd, static_cast<off_t>(total_bytes)) != 0) {
    last_errno_ = errno;
    close(fd);
    return false;
  }
  fd_ = fd;
  return MapAndBind_(fd, total_bytes, true);
#endif
}

bool ShmWriter::Open(const char* shm_name) {
  Close();
  last_errno_ = 0;
  create_symbol_count_ = 0;

  if (!shm_name || !*shm_name) {
    last_errno_ = EINVAL;
    return false;
  }

#if defined(_WIN32)
  HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
  if (!h) {
    last_errno_ = static_cast<int>(GetLastError());
    return false;
  }
  fd_ = h;
  // Map the entire region (bytes=0 means "entire mapping" on Windows).
  return MapAndBind_(0, 0, false);
#else
  int fd = shm_open(shm_name, O_RDWR, 0666);
  if (fd < 0) {
    last_errno_ = errno;
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    last_errno_ = errno;
    close(fd);
    return false;
  }
  const size_t bytes = static_cast<size_t>(st.st_size);
  fd_ = fd;
  return MapAndBind_(fd, bytes, false);
#endif
}

void ShmWriter::Close() {
  if (base_) {
#if defined(_WIN32)
    UnmapViewOfFile(base_);
#else
    munmap(base_, bytes_);
#endif
  }
  base_ = nullptr;
  bytes_ = 0;
  header_ = nullptr;
  symbol_dir_ = nullptr;
  entries_ = nullptr;

#if defined(_WIN32)
  if (fd_) {
    CloseHandle(fd_);
  }
  fd_ = nullptr;
#else
  if (fd_ >= 0) {
    close(fd_);
  }
  fd_ = -1;
#endif
}

bool ShmWriter::Unlink(const char* shm_name) {
#if defined(_WIN32)
  (void)shm_name;
  // Windows named shared memory segments are released when last handle closes.
  return true;
#else
  if (!shm_name || !*shm_name) {
    last_errno_ = EINVAL;
    return false;
  }
  if (shm_unlink(shm_name) != 0) {
    last_errno_ = errno;
    return false;
  }
  return true;
#endif
}

bool ShmWriter::MapAndBind_(int /*fd*/, size_t bytes, bool init_header) {
#if defined(_WIN32)
  void* p = MapViewOfFile(fd_, FILE_MAP_ALL_ACCESS, 0, 0, bytes == 0 ? 0 : bytes);
  if (!p) {
    last_errno_ = static_cast<int>(GetLastError());
    Close();
    return false;
  }
  base_ = p;
#else
  bytes_ = bytes;
  void* p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    last_errno_ = errno;
    Close();
    return false;
  }
  base_ = p;
#endif

  header_ = reinterpret_cast<ShmHeader*>(base_);
  symbol_dir_ = nullptr;

#if defined(_WIN32)
  if (bytes == 0) {
    bytes_ = static_cast<size_t>(header_->total_bytes);
  } else {
    bytes_ = bytes;
  }
#endif

  if (init_header) {
    ::memset(base_, 0, bytes_);
    InitHeader_(create_symbol_count_, bytes_);
    if (header_->symbol_dir_offset != 0 && header_->symbol_dir_bytes != 0) {
      symbol_dir_ = reinterpret_cast<char*>(base_) + static_cast<size_t>(header_->symbol_dir_offset);
    }
    entries_ = snapshot_table(base_, header_);
    InitSnapshotTable_(header_->symbol_count);
  } else {
    // Basic sanity bind: snapshot_offset is trusted only after ValidateHeader by caller.
  }

  if (!symbol_dir_ && header_ && header_->symbol_dir_offset != 0 && header_->symbol_dir_bytes != 0) {
    symbol_dir_ = reinterpret_cast<char*>(base_) + static_cast<size_t>(header_->symbol_dir_offset);
  }
  if (!entries_) {
    entries_ = snapshot_table(base_, header_);
  }
  return true;
}

void ShmWriter::InitHeader_(uint32_t symbol_count, size_t total_bytes) {
  // Fill ABI header.
  ShmHeader* h = header_;
  ::memset(h->magic, 0, sizeof(h->magic));
  const char kMagic[8] = {'M','D','G','A','T','E','1','\0'};
  ::memcpy(h->magic, kMagic, sizeof(kMagic));
  h->abi_version = 1;
  h->header_bytes = static_cast<uint32_t>(sizeof(ShmHeader));
  h->total_bytes = static_cast<uint64_t>(bytes_);
  h->endian = 1; // little
  h->flags = 1;  // has_snapshot (may be updated later if symbol_dir is enabled)

  h->writer_pid = GetPid();
  h->writer_uid = GetUid();
  h->writer_start_ns = NowMonotonicNs();
  store_u64_relaxed(&h->heartbeat_ns, 0);

  h->symbol_count = symbol_count;
  h->symbol_key_type = 1;
  const uint64_t header_aligned = static_cast<uint64_t>(align_up(sizeof(ShmHeader), kCacheLineBytes));
  const uint64_t dir_bytes = static_cast<uint64_t>(align_up(static_cast<size_t>(symbol_count) *
                                                                static_cast<size_t>(kSymbolDirEntryBytes),
                                                            kCacheLineBytes));
  h->symbol_dir_offset = header_aligned;
  h->symbol_dir_bytes = dir_bytes;

  h->snapshot_offset = header_aligned + dir_bytes;
  h->snapshot_entry_bytes = static_cast<uint32_t>(sizeof(SnapshotEntry));
  h->snapshot_payload_bytes = kMarketDataBytes;
  h->snapshot_mode = 1;
  h->snapshot_bytes = static_cast<uint64_t>(h->snapshot_entry_bytes) * static_cast<uint64_t>(h->symbol_count);

  h->event_ring_offset = 0;
  h->event_ring_bytes = 0;
  h->event_slot_bytes = 0;
  h->event_capacity = 0;
  store_u64_relaxed(&h->event_write_seq, 0);

  store_u32_relaxed(&h->md_status, 2); // RECONNECTING
  store_u32_relaxed(&h->last_err, 0);
  store_u64_relaxed(&h->last_md_ns, 0);

  // Flags: bit0=has_snapshot, bit1=has_symbol_dir
  h->flags = 1u | 2u;

  // Sanity check (debug): ensure layout matches allocated bytes.
  const uint64_t calc_total = h->snapshot_offset + h->snapshot_bytes;
  if (calc_total != static_cast<uint64_t>(total_bytes)) {
    // Keep header consistent even if caller ignores this mismatch.
    h->total_bytes = static_cast<uint64_t>(total_bytes);
  }
}

void ShmWriter::InitSnapshotTable_(uint32_t symbol_count) {
  if (!entries_) {
    entries_ = snapshot_table(base_, header_);
  }
  const size_t n = static_cast<size_t>(symbol_count);
  for (size_t i = 0; i < n; ++i) {
    store_u32_relaxed(&entries_[i].seq, 0);
    entries_[i].last_update_ns = 0;
    ::memset(&entries_[i].payload, 0, sizeof(entries_[i].payload));
  }
}

} // namespace mdg
