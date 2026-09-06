#pragma once

#include "../../data/candles.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

// Anchors are UTC, including the explicit 08:00 and 13:30 daily opens.
// Rolling windows use elapsed time, so tick/volume charts get the same window.
inline int64_t vwapSession(double ts, int mode) {
  using namespace std::chrono;
  if (!std::isfinite(ts) || ts < 0 || ts > 253402300799999.0)
    return std::numeric_limits<int64_t>::min();
  if (mode == 5) return 0;
  const int64_t offset = mode == 3 ? 8 * 3600000LL : mode == 4 ? 810 * 60000LL : 0;
  auto date = floor<days>(sys_time<milliseconds>{milliseconds{(int64_t)ts - offset}});
  if (mode == 1) date -= days{weekday{date}.iso_encoding() - 1};
  if (mode == 2) {
    const year_month_day civil{date};
    date = sys_days{civil.year() / civil.month() / day{1}};
  }
  return duration_cast<milliseconds>(date.time_since_epoch()).count() + offset;
}

struct VwapSeed {
  double origin = 0, weight = 0, sum = 0, squares = 0;
};

inline void updateVwapPoint(const Candle& c, VwapSeed seed,
                             float& mean, float& upper, float& lower) {
  const double w = c.aggVol > 0 ? c.aggVol : c.vol;
  const double p = (c.h + c.l + c.c) / 3 - seed.origin;
  if (w > 0 && std::isfinite(w) && std::isfinite(p)) {
    seed.weight += w; seed.sum += p * w; seed.squares += p * p * w;
  }
  mean = upper = lower = NAN;
  if (!(seed.weight > 0)) return;
  const double m = seed.sum / seed.weight;
  const double sd = std::sqrt(std::max(0.0, seed.squares / seed.weight - m * m));
  mean = (float)(seed.origin + m);
  upper = (float)(seed.origin + m + sd);
  lower = (float)(seed.origin + m - sd);
}

inline void computeVwapSeries(const CandleSeries& cs, int mode, int hours,
                              std::vector<float>& mean, std::vector<float>& upper,
                              std::vector<float>& lower, VwapSeed* tail = nullptr) {
  const size_t n = cs.v.size();
  mean.assign(n, NAN); upper.assign(n, NAN); lower.assign(n, NAN);
  if (tail) *tail = {};
  if (!n) return;
  // Offset the moments by a reference price to avoid cancellation at large
  // prices / tight spreads. Removal makes the rolling pass linear in bars.
  const double origin = cs.v.front().c;
  double weight = 0, sum = 0, squares = 0;
  size_t start = 0;
  int64_t session = std::numeric_limits<int64_t>::min();
  auto add = [&](size_t i, double sign) {
    const auto& c = cs.v[i];
    const double w = c.aggVol > 0 ? c.aggVol : c.vol;
    const double p = (c.h + c.l + c.c) / 3 - origin;
    if (!(w > 0) || !std::isfinite(w) || !std::isfinite(p)) return;
    weight += sign * w; sum += sign * p * w; squares += sign * p * p * w;
  };
  for (size_t i = 0; i < n; ++i) {
    const auto key = vwapSession(cs.v[i].ts, mode);
    if (key != session) {
      weight = sum = squares = 0; start = i; session = key;
    }
    if (mode == 5) {
      const double cutoff = cs.v[i].ts - std::clamp(hours, 1, 24 * 365) * 3600000.0;
      while (start < i && cs.v[start].ts <= cutoff) add(start++, -1);
    }
    const VwapSeed seed{origin, weight, sum, squares};
    if (tail && i + 1 == n) *tail = seed;
    updateVwapPoint(cs.v[i], seed, mean[i], upper[i], lower[i]);
    add(i, 1);
  }
}
