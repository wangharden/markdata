#pragma once

// Shared-memory ABI definitions for md_gateway <-> trade_app.
// Target: Linux x86_64, GCC 4.8, -std=c++14.
//
// Design goals:
// - Fixed-size snapshot table: entries[3000]
// - Per-entry SeqLock for lock-free writer + multi-reader
// - Cache-line alignment to reduce false sharing
// - Avoid dynamic allocation / exceptions on hot paths
//
// NOTE (重要):
// - “SeqLock + 非原子 payload”在严格 ISO C++ 内存模型下可能被视为数据竞争(UB)。
//   但在实际 HFT 系统中常用，且在 x86_64 + GCC 上通常工作正常。
// - 若你要做到“严格无数据竞争”，建议后续切换为：entry 双缓冲 + 版本号，或 payload 按 8B 原子块读写。

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mdg {

static const uint32_t kCacheLineBytes = 64;
static const uint32_t kMaxSymbols = 3000;
static const uint32_t kMarketDataBytes = 320;  // 你确认 MarketData 对齐后大小为 320B
static const uint32_t kWindCodeBytes = 16;     // fixed wind_code buffer (e.g. "600000.SH\0")
static const uint32_t kSymbolDirEntryBytes = kWindCodeBytes; // id->wind_code directory entry size

// -------------------------
// Minimal atomic wrappers (POD) for SHM
// -------------------------
//
// Rationale:
// - std::atomic placed in mmap'ed SHM has object-lifetime/ABI caveats in pure ISO C++.
// - GCC __atomic builtins operate on plain integers and are widely used for SHM ABIs.

struct AtomicU32 {
  uint32_t v;
};

struct AtomicU64 {
  uint64_t v;
};

inline uint32_t load_u32_relaxed(const AtomicU32* a) {
#if defined(_MSC_VER)
  // Interlocked* provides full fence; treat as relaxed on x86/x64 by using plain read.
  return a->v;
#else
  return __atomic_load_n(&a->v, __ATOMIC_RELAXED);
#endif
}
inline uint32_t load_u32_acquire(const AtomicU32* a) {
#if defined(_MSC_VER)
  return static_cast<uint32_t>(
      _InterlockedCompareExchange(reinterpret_cast<volatile long*>(&const_cast<AtomicU32*>(a)->v), 0, 0));
#else
  return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
#endif
}
inline void store_u32_relaxed(AtomicU32* a, uint32_t x) {
#if defined(_MSC_VER)
  a->v = x;
#else
  __atomic_store_n(&a->v, x, __ATOMIC_RELAXED);
#endif
}
inline void store_u32_release(AtomicU32* a, uint32_t x) {
#if defined(_MSC_VER)
  _InterlockedExchange(reinterpret_cast<volatile long*>(&a->v), static_cast<long>(x));
#else
  __atomic_store_n(&a->v, x, __ATOMIC_RELEASE);
#endif
}
inline uint32_t fetch_add_u32_relaxed(AtomicU32* a, uint32_t d) {
#if defined(_MSC_VER)
  return static_cast<uint32_t>(
      _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(&a->v), static_cast<long>(d)));
#else
  return __atomic_fetch_add(&a->v, d, __ATOMIC_RELAXED);
#endif
}

inline uint64_t load_u64_relaxed(const AtomicU64* a) {
#if defined(_MSC_VER)
  return a->v;
#else
  return __atomic_load_n(&a->v, __ATOMIC_RELAXED);
#endif
}
inline uint64_t load_u64_acquire(const AtomicU64* a) {
#if defined(_MSC_VER)
  return static_cast<uint64_t>(
      _InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(&const_cast<AtomicU64*>(a)->v), 0, 0));
#else
  return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
#endif
}
inline void store_u64_relaxed(AtomicU64* a, uint64_t x) {
#if defined(_MSC_VER)
  a->v = x;
#else
  __atomic_store_n(&a->v, x, __ATOMIC_RELAXED);
#endif
}
inline void store_u64_release(AtomicU64* a, uint64_t x) {
#if defined(_MSC_VER)
  _InterlockedExchange64(reinterpret_cast<volatile long long*>(&a->v), static_cast<long long>(x));
#else
  __atomic_store_n(&a->v, x, __ATOMIC_RELEASE);
#endif
}
inline uint64_t fetch_add_u64_relaxed(AtomicU64* a, uint64_t d) {
#if defined(_MSC_VER)
  return static_cast<uint64_t>(
      _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(&a->v), static_cast<long long>(d)));
#else
  return __atomic_fetch_add(&a->v, d, __ATOMIC_RELAXED);
#endif
}

inline void compiler_barrier() {
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

// -------------------------
// Payload ABI (320B)
// -------------------------
//
// 建议：SHM payload 与内部结构解耦。
// - gateway 内部可用 richer struct
// - 写入 SHM 时 pack/copy 到这个 320B payload
// trade_app 可直接使用该 payload，或 decode 成内部结构。

struct alignas(kCacheLineBytes) MarketData320 {
  uint8_t bytes[kMarketDataBytes];
};

static_assert(sizeof(MarketData320) == kMarketDataBytes, "MarketData320 size mismatch");
static_assert((kMarketDataBytes % kCacheLineBytes) == 0, "MarketData320 must be cacheline-multiple");

// -------------------------
// SHM Header (ABI)
// -------------------------
//
// NOTE:
// - 头部字段尽量保持 fixed layout，新增字段放 reserved。
// - heartbeat_ns: writer 周期刷新；reader 用于健康检测/重连。

struct alignas(kCacheLineBytes) ShmHeader {
  // --- ABI / 校验 ---
  char     magic[8];        // "MDGATE1\0"
  uint32_t abi_version;     // 1
  uint32_t header_bytes;    // sizeof(ShmHeader)
  uint64_t total_bytes;     // total shm bytes
  uint32_t endian;          // 1=little
  uint32_t flags;           // bit flags

  // --- 写端身份/生命周期 ---
  uint32_t writer_pid;
  uint32_t writer_uid;      // optional
  uint64_t writer_start_ns; // CLOCK_MONOTONIC ns at start (acts like epoch)
  AtomicU64 heartbeat_ns;   // periodically updated

  // --- 符号目录（可选，弱依赖） ---
  // 当 trade_app 不想依赖本地 CSV/配置时，可从 shm 构建 id->wind_code 映射。
  uint32_t symbol_count;       // expected 3000
  uint32_t symbol_key_type;    // 1=wind_code string, 2=hash, ...
  uint64_t symbol_dir_offset;  // 0 means absent
  uint64_t symbol_dir_bytes;   // 0 means absent

  // --- 快照表（核心） ---
  uint64_t snapshot_offset;    // offset to snapshot entries[]
  uint64_t snapshot_bytes;     // bytes of snapshot table
  uint32_t snapshot_entry_bytes;
  uint32_t snapshot_payload_bytes; // 320
  uint32_t snapshot_mode;      // 1=per-entry seqlock, 2=double-buffer per-entry, ...
  uint32_t reserved0;

  // --- 预留：event ring (未来增量/逐笔) ---
  uint64_t event_ring_offset;
  uint64_t event_ring_bytes;
  uint32_t event_slot_bytes;
  uint32_t event_capacity;
  AtomicU64 event_write_seq;

  // --- 状态 ---
  AtomicU32 md_status;      // 0=OK, 1=DISCONNECTED, 2=RECONNECTING...
  AtomicU32 last_err;       // last error code
  AtomicU64 last_md_ns;     // last marketdata time (monotonic ns)

  uint64_t reserved[8];
};

// -------------------------
// Snapshot Entry (SeqLock)
// -------------------------
//
// Entry layout:
// [ meta cacheline (64B) | payload (320B = 5 cachelines) ] => 384B
// - seq: odd=writing, even=stable
// - payload is aligned at cacheline boundary (offset 64)

struct alignas(kCacheLineBytes) SnapshotEntry {
  AtomicU32 seq;            // seqlock counter
  uint32_t _pad0;
  uint64_t last_update_ns;  // writer-stamped monotonic ns (optional)
  uint8_t  meta_pad[48];    // pad meta to 64B

  MarketData320 payload;    // 320B
};

static_assert(offsetof(SnapshotEntry, payload) == kCacheLineBytes, "payload must be cacheline-aligned");
static_assert(sizeof(SnapshotEntry) == (kCacheLineBytes + kMarketDataBytes), "SnapshotEntry size mismatch");

// -------------------------
// SeqLock helpers
// -------------------------
//
// Writer:
//   odd = seqlock_write_begin(seq)
//   write payload/metadata
//   seqlock_write_end(seq, odd)
//
// Reader:
//   loop:
//     s1 = load(seq) [acquire]
//     if odd -> retry
//     copy payload
//     s2 = load(seq) [acquire]
//     if s1 == s2 -> success else retry

inline uint32_t seqlock_write_begin(AtomicU32* seq) {
  // Make it odd.
  uint32_t odd = fetch_add_u32_relaxed(seq, 1) + 1;
  compiler_barrier();
  return odd;
}

inline void seqlock_write_end(AtomicU32* seq, uint32_t odd) {
  compiler_barrier();
  // Publish stable (even). Release makes preceding payload writes visible.
  store_u32_release(seq, odd + 1);
}

inline bool seqlock_read_once(const SnapshotEntry* e, MarketData320* out, uint32_t* out_seq_even) {
  const uint32_t s1 = load_u32_acquire(&e->seq);
  if (s1 & 1U) return false;

  compiler_barrier();
  // Copy payload (320B). Use builtin to encourage inline/rep-mov.
  ::memcpy(out, &e->payload, sizeof(MarketData320));
  compiler_barrier();

  const uint32_t s2 = load_u32_acquire(&e->seq);
  if (s1 != s2) return false;
  if (out_seq_even) *out_seq_even = s2;
  return true;
}

// -------------------------
// Layout helpers
// -------------------------

inline size_t align_up(size_t x, size_t a) {
  return (x + (a - 1)) & ~(a - 1);
}

inline SnapshotEntry* snapshot_table(void* shm_base, const ShmHeader* h) {
  return reinterpret_cast<SnapshotEntry*>(reinterpret_cast<uint8_t*>(shm_base) + h->snapshot_offset);
}

inline const SnapshotEntry* snapshot_table(const void* shm_base, const ShmHeader* h) {
  return reinterpret_cast<const SnapshotEntry*>(reinterpret_cast<const uint8_t*>(shm_base) + h->snapshot_offset);
}

} // namespace mdg
