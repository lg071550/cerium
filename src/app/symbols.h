#pragma once

// The tradable symbol set and its per-symbol venue support counts — a
// contract with SYMBOLS in feeds/registry.ts. Index order matters (it is
// referenced by wire commands and stored settings). tools/check-venues.mjs
// verifies both literals against the TS registry at build time, so adding a
// symbol or venue without updating this file fails the build instead of
// rendering wrong labels/counts forever.
namespace symbols {

inline constexpr const char* kNames[] = {"ETH", "BTC", "SOL"};
inline constexpr int kCount = 3;
inline constexpr int kVenueCounts[kCount] = {27, 24, 24};

} // namespace symbols
