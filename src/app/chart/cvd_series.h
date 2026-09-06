#pragma once

#include "../../data/orderflow.h"
#include <algorithm>
#include <cmath>

inline bool cvdTradePass(const OrderFlowTrade& t, double minUsd, double maxUsd) {
  const double usd = t.price * t.qty;
  return std::isfinite(usd) && usd > 0 &&
         (minUsd <= 0 || usd >= minUsd) && (maxUsd <= 0 || usd < maxUsd);
}

inline void buildCvdSeries(const CandleSeries& cs, const OrderFlowSeries* flow,
                           double minUsd, double maxUsd,
                           std::vector<float>& close, std::vector<float>& high,
                           std::vector<float>& low) {
  const size_t n = cs.v.size();
  close.assign(n, NAN); high.assign(n, NAN); low.assign(n, NAN);
  if (!n) return;
  const bool filtered = minUsd > 0 || maxUsd > 0;
  double acc = 0;
  if (!filtered) {
    // OHLCV history contains each bar's full delta. A display-style change
    // must not silently replace that history with a short raw-print window.
    for (size_t i = 0; i < n; ++i) {
      const double open = acc;
      acc += cs.v[i].delta;
      close[i] = (float)acc;
      high[i] = (float)std::max(open, acc);
      low[i] = (float)std::min(open, acc);
    }
    return;
  }
  if (!flow || flow->v.empty()) return;
  // Bars before coverage are unknown, not zero. The first covered candle is
  // partial: display the trades we do have, including a live-only bootstrap.
  const double firstTs = flow->v.front().ts;
  const auto next = std::upper_bound(cs.v.begin(), cs.v.end(), firstTs,
      [](double ts, const Candle& c) { return ts < c.ts; });
  const size_t first = next == cs.v.begin() ? 0 : (size_t)(next - cs.v.begin() - 1);
  std::vector<double> delta(n), hi(n), lo(n);
  for (const auto& t : flow->v) {
    if (!cvdTradePass(t, minUsd, maxUsd) || t.ts < cs.v[first].ts || t.ts < firstTs) continue;
    // Live venues can arrive out of timestamp order. Assign each print to its
    // actual bar rather than advancing a cursor past other venues' prints.
    const auto end = std::upper_bound(cs.v.begin() + first, cs.v.end(), t.ts,
        [](double ts, const Candle& c) { return ts < c.ts; });
    const size_t i = (size_t)(end - cs.v.begin() - 1);
    delta[i] += t.side == 0 ? t.qty : -t.qty;
    hi[i] = std::max(hi[i], delta[i]); lo[i] = std::min(lo[i], delta[i]);
  }
  for (size_t i = first; i < n; ++i) {
    high[i] = (float)(acc + hi[i]); low[i] = (float)(acc + lo[i]);
    acc += delta[i]; close[i] = (float)acc;
  }
}
