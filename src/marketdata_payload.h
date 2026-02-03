#pragma once

// Fixed-size (320B) payload stored in SnapshotEntry::payload.bytes.
//
// Notes:
// - All numeric fields are little-endian (host order on x86/x64).
// - Prices follow TDF convention: int64 in 1/10000 units.
// - No pointers / std::string / dynamic members.
//
// Versioning:
// - payload_version allows extending semantics while keeping 320B size via reserved fields.

#include <stdint.h>
#include <stddef.h>

namespace mdg {

struct MarketDataPayloadV1 {
  uint32_t payload_version;   // 1
  uint32_t flags;             // bit0=valid

  int32_t action_day;         // yyyymmdd (from TDF)
  int32_t trading_day;        // yyyymmdd (from TDF)
  int32_t time_hhmmssmmm;     // HHMMSSmmm (from TDF)
  int32_t status;             // from TDF

  int64_t pre_close_x10000;
  int64_t open_x10000;
  int64_t high_x10000;
  int64_t low_x10000;
  int64_t last_x10000;        // latest match

  int64_t high_limit_x10000;
  int64_t low_limit_x10000;

  int64_t volume;             // iVolume
  int64_t turnover;           // iTurnover

  int64_t bid_price_x10000[5];
  int64_t bid_vol[5];
  int64_t ask_price_x10000[5];
  int64_t ask_vol[5];

  char wind_code[16];         // e.g. "600000.SH"
  char prefix[8];             // from TDF_MARKET_DATA::chPrefix (if available)

  uint64_t recv_ns;           // gateway monotonic timestamp

  uint64_t reserved[4];       // keep total = 320B
};

static_assert(sizeof(MarketDataPayloadV1) == 320, "MarketDataPayloadV1 must be 320B");

static inline void ZeroMarketDataPayload(MarketDataPayloadV1* p) {
  if (!p) return;
  // Portable memset is fine (POD).
  unsigned char* b = reinterpret_cast<unsigned char*>(p);
  for (size_t i = 0; i < sizeof(MarketDataPayloadV1); ++i) b[i] = 0;
}

} // namespace mdg
