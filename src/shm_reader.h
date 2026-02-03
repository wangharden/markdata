#pragma once

// SHM reader side: trade_app maps the SHM region (read-only) and reads latest snapshots.
//
// Typical usage:
//   mdg::ShmReader r;
//   r.Open("/md_gate_shm");
//   mdg::MarketData320 md;
//   r.ReadSnapshot(symbol_id, &md);

#include "struct_def.h"

#include <stdint.h>
#include <stddef.h>

namespace mdg {

class ShmReader {
public:
  ShmReader();
  ~ShmReader();

  ShmReader(const ShmReader&) = delete;
  ShmReader& operator=(const ShmReader&) = delete;

  // Open SHM read-only (shm_open + mmap PROT_READ).
  bool Open(const char* shm_name);
  void Close();

  int last_errno() const { return last_errno_; }

  const void* base() const { return base_; }
  size_t bytes() const { return bytes_; }
  const ShmHeader* header() const { return header_; }
  const SnapshotEntry* entries() const { return entries_; }

  uint32_t symbol_count() const { return header_ ? header_->symbol_count : 0; }

  // Health checks
  inline uint64_t heartbeat_ns() const { return header_ ? load_u64_acquire(&header_->heartbeat_ns) : 0; }
  inline uint32_t md_status() const { return header_ ? load_u32_acquire(&header_->md_status) : 0; }
  inline uint32_t last_err() const { return header_ ? load_u32_acquire(&header_->last_err) : 0; }
  inline uint64_t writer_start_ns() const { return header_ ? header_->writer_start_ns : 0; }

  // Read latest snapshot with seqlock retry.
  // - returns true on success; false if retries exceeded.
  // - out_seq_even: optional, the even seq observed.
  bool ReadSnapshot(uint32_t symbol_id, MarketData320* out, uint32_t* out_seq_even);

  // Convenience: best-effort read with bounded spins.
  bool ReadSnapshotSpin(uint32_t symbol_id, MarketData320* out, uint32_t max_spins, uint32_t* out_seq_even);

  // Validate header ABI (magic/version/size).
  bool ValidateHeader() const;

private:
  bool MapAndBind_(int fd, size_t bytes);

private:
  const void* base_;
  size_t bytes_;
  const ShmHeader* header_;
  const SnapshotEntry* entries_;
#if defined(_WIN32)
  void* fd_; // HANDLE
#else
  int fd_;
#endif
  int last_errno_;
};

} // namespace mdg
