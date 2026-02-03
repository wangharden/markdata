#pragma once

// SHM writer side: md_gateway creates/initializes the SHM region and updates snapshot entries.
//
// This header is intentionally "outline-first":
// - keep ABI in struct_def.h
// - keep syscalls thin and explicit
// - hot path is UpdateSnapshot() (seqlock + memcpy)

#include "struct_def.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

namespace mdg {

class ShmWriter {
public:
  ShmWriter();
  ~ShmWriter();

  ShmWriter(const ShmWriter&) = delete;
  ShmWriter& operator=(const ShmWriter&) = delete;

  // Create new SHM (shm_open + ftruncate + mmap) and initialize header/table.
  // Returns false on failure; caller can inspect last_errno().
  bool Create(const char* shm_name, uint32_t symbol_count);

  // Open existing SHM for write (rare; mainly for debug/re-attach).
  bool Open(const char* shm_name);

  void Close();
  bool Unlink(const char* shm_name); // shm_unlink

  int last_errno() const { return last_errno_; }

  void* base() const { return base_; }
  size_t bytes() const { return bytes_; }
  ShmHeader* header() const { return header_; }
  SnapshotEntry* entries() const { return entries_; }
  char* symbol_dir() const { return symbol_dir_; }

  // Hot path: write one symbol snapshot (320B) with seqlock publish.
  // - now_ns: CLOCK_MONOTONIC timestamp from gateway
  inline void UpdateSnapshot(uint32_t symbol_id, const MarketData320& md, uint64_t now_ns) {
    assert(header_ != nullptr);
    if (header_ && symbol_id >= header_->symbol_count) {
      return;
    }
    SnapshotEntry* e = &entries_[symbol_id];
    const uint32_t odd = seqlock_write_begin(&e->seq);
    e->last_update_ns = now_ns;
    ::memcpy(&e->payload, &md, sizeof(MarketData320));
    seqlock_write_end(&e->seq, odd);
  }

  // Update gateway heartbeat (reader health check).
  inline void UpdateHeartbeat(uint64_t now_ns) {
    store_u64_release(&header_->heartbeat_ns, now_ns);
  }

  inline void UpdateLastMdNs(uint64_t now_ns) {
    store_u64_release(&header_->last_md_ns, now_ns);
  }

  // Optional: publish md_status / last_err (non-hot path).
  inline void SetMdStatus(uint32_t status) { store_u32_release(&header_->md_status, status); }
  inline void SetLastErr(uint32_t err) { store_u32_release(&header_->last_err, err); }

  // Publish symbol_dir entry (id -> wind_code). Each entry is kSymbolDirEntryBytes bytes.
  // Safe to call after Create() (SHM is writable). Not on the hot path.
  inline void WriteSymbolDirEntry(uint32_t symbol_id, const char* wind_code) {
    if (!header_ || !symbol_dir_) return;
    if (header_->symbol_dir_offset == 0 || header_->symbol_dir_bytes == 0) return;
    if (symbol_id >= header_->symbol_count) return;
    char* dst = symbol_dir_ + static_cast<size_t>(symbol_id) * static_cast<size_t>(kSymbolDirEntryBytes);
    ::memset(dst, 0, kSymbolDirEntryBytes);
    if (!wind_code) return;
    // Copy up to kSymbolDirEntryBytes-1 bytes to keep '\0' termination.
    for (uint32_t i = 0; i + 1 < kSymbolDirEntryBytes && wind_code[i] != '\0'; ++i) {
      dst[i] = wind_code[i];
    }
  }

private:
  bool MapAndBind_(int fd, size_t bytes, bool init_header);
  void InitHeader_(uint32_t symbol_count, size_t total_bytes);
  void InitSnapshotTable_(uint32_t symbol_count);

private:
  void* base_;
  size_t bytes_;
  ShmHeader* header_;
  char* symbol_dir_;
  SnapshotEntry* entries_;
  uint32_t create_symbol_count_;
#if defined(_WIN32)
  void* fd_; // HANDLE
#else
  int fd_;
#endif
  int last_errno_;
};

} // namespace mdg
