#pragma once

#include <cstdint>
#include <vector>

// Binned HyperTracker liquidation / stop-loss book for the active symbol.
// Worker fetches + bins; the chart draws this as a live right-edge profile.
// There is no time-series field — HyperTracker history is quota-expensive
// and the free tier cannot match a full HL heatmap.
struct HtBand {
  float lo = 0, hi = 0;
  float longUsd = 0, shortUsd = 0;
};

struct HtLayer {
  std::vector<HtBand> bands;
  double fetchedAt = 0; // unix ms
  float ref = 1;        // p95 cell notional for heat ramps
};

struct HtMaps {
  static constexpr int kOk = 0;
  static constexpr int kNoToken = 1;
  static constexpr int kQuota = 2;
  static constexpr int kError = 3;

  int sym = -1;
  int status = kNoToken;
  int used = 0;
  int quota = 100;
  HtLayer liq, sl;
  uint64_t version = 0;

  void load(const double* liqRows, int liqN, const double* slRows, int slN, int symIdx,
            int st, int usedToday, int quotaDay, double liqAt, double slAt, float liqRef,
            float slRef);
  void clear();
};
