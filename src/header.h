#pragma once

// Compatibility header:
// - The authoritative SHM ABI types live in `gate_result/src/struct_def.h` under namespace `mdg`.
// - This file provides simple aliases for legacy includes / docs that reference `header.h`.

#include "struct_def.h"

using ShmHeader = mdg::ShmHeader;
using SnapshotEntry = mdg::SnapshotEntry;
using MarketData320 = mdg::MarketData320;
