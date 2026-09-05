#pragma once

#include "candles.h"
#include "heatmap.h"
#include "market.h"

#include <cstdint>
#include <vector>

// Predicted liquidation map: event-sourced density, not exchange liq prices.
// Each bar copies surviving bins forward, zeros anything the candle traded
// through, then stamps new weight at isolated-leverage offsets of that bar's
// entry when flow looks like fresh leverage (delta spike, OI not shrinking).
// Columns share a price lattice so the field lines up across time; the candle
// path is empty air because those rows were swept.
struct LiqMapSeries {
  static constexpr int kMaxRows = 4096;
  static constexpr int kLookback = 20;
  static constexpr uint32_t kBand10 = 1u;
  static constexpr uint32_t kBand25 = 2u;
  static constexpr uint32_t kBand50 = 4u;
  static constexpr uint32_t kBand100 = 8u;
  static constexpr uint32_t kBandAll = 15u;

  double bin = 0;
  std::vector<HeatmapBar> bars;
  float ref = 0;
  int sym = -1;
  int tfKind = -1;
  double tfValue = 0;
  uint64_t builtShape = ~0ull;
  size_t builtOiHist = 0;
  uint32_t builtBands = 0;

  void clear();
  // Full walk. `bands` is a bitmask of kBand*. Empty `mkt` still stamps from
  // delta; OI/funding only gate and weight when present.
  void rebuild(const CandleSeries& cs, const MarketSeries* mkt, uint32_t bands);
  // Forming-bar path: recopy from the last closed column, resweep, restamp.
  // False → caller should rebuild (length/identity mismatch).
  bool updateLast(const CandleSeries& cs, const MarketSeries* mkt, uint32_t bands);
};

double liqMapNiceStep(double raw);
// AUTO grouping: ~12.5 bps of mid (BTC ≈ $100), snapped once per symbol/TF.
// Chart code locks the first pick so mid/nice-step drift cannot regrid.
double liqMapAutoBin(double mid, double nativeTick);
// Manual BIN slider % → dollar grouping, same log sweep as BOOK HEAT's BIN.
double liqMapBinUsd(int sel);
