#pragma once

#include "candles.h"

#include <cstdint>
#include <vector>

// Aggressor prints used by footprint chart modes. This is separate from the
// multi-venue Tape ring: the chart needs a stable historical window whose
// symbol/timeframe identity can be validated during async loads. Live prints
// from every mask-selected venue are rebased onto the Binance reference axis
// (print - venueMid + refMid) before they land here, so mixed-venue footprints
// don't scatter across inter-venue mid drift.
struct OrderFlowTrade {
  double ts = 0;
  double price = 0;
  double qty = 0;
  uint8_t side = 0; // 0 = taker buy, 1 = taker sell
};

struct OrderFlowSeries {
  static constexpr size_t MAX_TRADES = 320000;

  std::vector<OrderFlowTrade> v;
  Timeframe tf;
  int sym = -1;
  uint64_t version = 0;
  uint64_t generation = 0; // history replacement/compaction, not a tail append
  // Cumulative net front-index shift since the last generation bump:
  // prepends push indices up, front-compaction pulls them down. Indicator
  // cursors survive both by applying (indexShift - cursor's seen shift);
  // replace/clear zero it and bump generation, which invalidates outright.
  int64_t indexShift = 0;

  // Packed rows: [ts, price, qty, side] x n. `prepend` stitches an older
  // aggTrade page in front of the window already loaded (progressive fetch).
  // `prepends` increments only on that path so CLUSTER can fold older prints
  // into existing cells instead of hashing the whole window again.
  uint64_t prepends = 0;
  void load(const double* data, int n, Timeframe tf_, int symIdx, bool prepend = false);
  void onTrade(double price, double qty, int side, double tsMs);
  void clear();
};
