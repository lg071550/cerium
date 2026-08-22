#pragma once

#include <cstdint>
#include <vector>

// Aggregated derivatives market data for future indicators: open interest
// (base-asset contracts, summed across perp venues), a median funding rate,
// and liquidation prints. Values are timestamped samples, not candle-aligned
// — consumers resample onto the candle axis when plotting. History is 5m;
// the live tail is ~1s ticker snapshots.
struct OiSample {
  double ts = 0;  // ms
  double oi = 0;  // summed base-asset OI across venues
};

struct FundingSample {
  double ts = 0;   // ms
  double rate = 0; // median perp funding rate (per interval, signed)
};

struct LiqPrint {
  double ts = 0, price = 0, qty = 0;
  uint8_t side = 0; // aggressor side: 0 = buy (short liq), 1 = sell (long liq)
};

struct MarketSeries {
  static constexpr size_t MAX_SAMPLES = 16384; // 5m hist + 1s live tail

  std::vector<OiSample> oi;
  std::vector<FundingSample> funding;
  std::vector<LiqPrint> liq;
  int64_t liqTop = 0; // worker-global index one past the newest known liq row
  int sym = -1;
  uint64_t version = 0;
  // Bumps only when the liq series changes — OI/funding snapshots arrive
  // ~1/s and bump `version`, which would otherwise force consumers (the
  // liquidations panel's filter) to re-scan 16k prints for nothing.
  uint64_t liqVersion = 0;

  // Packed rows: oi [ts, coin] x n, funding [ts, rate] x n, liq [ts, price,
  // qty, side] x n. Each load replaces its buffer wholesale (worker retains
  // the rolling history and re-posts it); `base` is the worker-global index
  // of the first retained liq row. Deltas append in arrival order and dedupe
  // against liqTop, so per-print posts never duplicate a full sync.
  void loadOi(const double* data, int n, int symIdx);
  void loadFunding(const double* data, int n, int symIdx);
  void loadLiq(const double* data, int n, int symIdx, int64_t base);
  void appendLiq(const double* data, int n, int64_t start, int symIdx);
  void clear();
};
