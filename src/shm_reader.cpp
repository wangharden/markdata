#include "shm_reader.h"

#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mdg {

ShmReader::ShmReader()
    : base_(nullptr),
      bytes_(0),
      header_(nullptr),
      entries_(nullptr),
#if defined(_WIN32)
      fd_(nullptr),
#else
      fd_(-1),
#endif
      last_errno_(0) {}

ShmReader::~ShmReader() { Close(); }

bool ShmReader::Open(const char* shm_name) {
  Close();
  last_errno_ = 0;

  if (!shm_name || !*shm_name) {
    last_errno_ = EINVAL;
    return false;
  }

#if defined(_WIN32)
  HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, shm_name);
  if (!h) {
    last_errno_ = static_cast<int>(GetLastError());
    return false;
  }
  fd_ = h;
  // Map the entire region (bytes=0 means "entire mapping" on Windows).
  return MapAndBind_(0, 0);
#else
  int fd = shm_open(shm_name, O_RDONLY, 0666);
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
  return MapAndBind_(fd, bytes);
#endif
}

void ShmReader::Close() {
  if (base_) {
#if defined(_WIN32)
    UnmapViewOfFile(base_);
#else
    munmap(const_cast<void*>(base_), bytes_);
#endif
  }
  base_ = nullptr;
  bytes_ = 0;
  header_ = nullptr;
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

bool ShmReader::ValidateHeader() const {
  if (!header_) return false;

  const char kMagic[8] = {'M','D','G','A','T','E','1','\0'};
  if (::memcmp(header_->magic, kMagic, sizeof(kMagic)) != 0) return false;
  if (header_->abi_version != 1) return false;
  if (header_->endian != 1) return false;
  if (header_->header_bytes < sizeof(ShmHeader)) return false;
  if (header_->snapshot_entry_bytes != sizeof(SnapshotEntry)) return false;
  if (header_->snapshot_payload_bytes != kMarketDataBytes) return false;

  const uint64_t total_bytes = header_->total_bytes;
  if (total_bytes == 0) return false;
  if (bytes_ < total_bytes) return false;
  const uint64_t snapshot_end = header_->snapshot_offset + header_->snapshot_bytes;
  if (snapshot_end > total_bytes) return false;
  if (header_->symbol_count == 0 || header_->symbol_count > kMaxSymbols) return false;
  if (header_->snapshot_bytes !=
      (static_cast<uint64_t>(header_->symbol_count) * static_cast<uint64_t>(header_->snapshot_entry_bytes))) {
    return false;
  }

  // Optional symbol_dir validation (id->wind_code directory).
  if (header_->symbol_dir_offset != 0 || header_->symbol_dir_bytes != 0) {
    if (header_->symbol_key_type != 1) return false;
    if (header_->symbol_dir_offset < header_->header_bytes) return false;
    const uint64_t dir_end = header_->symbol_dir_offset + header_->symbol_dir_bytes;
    if (dir_end > total_bytes) return false;
    const uint64_t min_bytes = static_cast<uint64_t>(header_->symbol_count) * static_cast<uint64_t>(kSymbolDirEntryBytes);
    if (header_->symbol_dir_bytes < min_bytes) return false;
    if (header_->snapshot_offset < dir_end) return false;
  }
  return true;
}

bool ShmReader::ReadSnapshot(uint32_t symbol_id, MarketData320* out, uint32_t* out_seq_even) {
  return ReadSnapshotSpin(symbol_id, out, 200, out_seq_even);
}

bool ShmReader::ReadSnapshotSpin(uint32_t symbol_id, MarketData320* out, uint32_t max_spins, uint32_t* out_seq_even) {
  if (!entries_ || !header_ || !out) return false;
  if (symbol_id >= header_->symbol_count) return false;

  const SnapshotEntry* e = &entries_[symbol_id];
  for (uint32_t i = 0; i < max_spins; ++i) {
    if (seqlock_read_once(e, out, out_seq_even)) return true;
  }
  return false;
}

bool ShmReader::MapAndBind_(int /*fd*/, size_t bytes) {
#if defined(_WIN32)
  void* p = MapViewOfFile(fd_, FILE_MAP_READ, 0, 0, bytes == 0 ? 0 : bytes);
  if (!p) {
    last_errno_ = static_cast<int>(GetLastError());
    Close();
    return false;
  }
  base_ = p;
#else
  bytes_ = bytes;
  void* p = mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    last_errno_ = errno;
    Close();
    return false;
  }
  base_ = p;
#endif

  header_ = reinterpret_cast<const ShmHeader*>(base_);

#if defined(_WIN32)
  if (bytes == 0) {
    bytes_ = static_cast<size_t>(header_->total_bytes);
  } else {
    bytes_ = bytes;
  }
#endif

  entries_ = snapshot_table(base_, header_);
  return true;
}

} // namespace mdg
