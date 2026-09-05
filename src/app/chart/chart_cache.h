#pragma once

#include "../../data/candles.h"

#include <bit>
#include <cstdint>

inline uint64_t chartMix(uint64_t hash, uint64_t value) {
  return (hash ^ value) * 1099511628211ull;
}

inline uint64_t chartSeriesIdentity(const CandleSeries& cs) {
  uint64_t hash = chartMix(1469598103934665603ull, (uint64_t)(cs.sym + 1));
  hash = chartMix(hash, (uint64_t)(cs.tf.kind + 1));
  return chartMix(hash, std::bit_cast<uint64_t>(cs.tf.value));
}

inline uint64_t candleSig(const CandleSeries& cs) {
  uint64_t hash = chartMix(chartSeriesIdentity(cs), cs.historyVersion);
  hash = chartMix(hash, cs.v.size());
  if (!cs.v.empty()) {
    const Candle& c = cs.v.back();
    for (double value : {c.ts, c.o, c.h, c.l, c.c, c.vol, c.delta, c.aggVol})
      hash = chartMix(hash, std::bit_cast<uint64_t>(value));
  }
  return hash;
}

inline uint64_t shapeSig(const CandleSeries& cs) {
  uint64_t hash = chartMix(chartSeriesIdentity(cs), cs.historyVersion);
  hash = chartMix(hash, cs.v.size());
  if (!cs.v.empty()) {
    hash = chartMix(hash, std::bit_cast<uint64_t>(cs.v.front().ts));
    hash = chartMix(hash, std::bit_cast<uint64_t>(cs.v.back().ts));
  }
  return hash;
}

// Appending a bar does not move existing footprint indices; prepending or
// trimming the front does. History corrections are tracked by shapeSig.
inline uint64_t axisSig(const CandleSeries& cs) {
  uint64_t hash = chartSeriesIdentity(cs);
  return cs.v.empty() ? hash : chartMix(hash, std::bit_cast<uint64_t>(cs.v.front().ts));
}
