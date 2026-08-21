#include "market.h"

#include <algorithm>

void MarketSeries::loadOi(const double* data, int n, int symIdx) {
  sym = symIdx;
  n = std::max(0, n);
  oi.clear();
  if (!data || n <= 0) { ++version; return; }
  int first = std::max(0, n - (int)MAX_SAMPLES);
  oi.reserve((size_t)(n - first));
  for (int i = first; i < n; ++i) {
    const double* row = data + (size_t)i * 2;
    if (!(row[0] > 0) || !(row[1] > 0)) continue;
    oi.push_back({row[0], row[1]});
  }
  ++version;
}

void MarketSeries::loadFunding(const double* data, int n, int symIdx) {
  sym = symIdx;
  n = std::max(0, n);
  funding.clear();
  if (!data || n <= 0) { ++version; return; }
  int first = std::max(0, n - (int)MAX_SAMPLES);
  funding.reserve((size_t)(n - first));
  for (int i = first; i < n; ++i) {
    const double* row = data + (size_t)i * 2;
    if (!(row[0] > 0)) continue;
    funding.push_back({row[0], row[1]});
  }
  ++version;
}

void MarketSeries::loadLiq(const double* data, int n, int symIdx, int64_t base) {
  sym = symIdx;
  n = std::max(0, n);
  liq.clear();
  if (!data || n <= 0) {
    liqTop = base + n;
    ++version;
    return;
  }
  int first = std::max(0, n - (int)MAX_SAMPLES);
  liq.reserve((size_t)(n - first));
  for (int i = first; i < n; ++i) {
    const double* row = data + (size_t)i * 4;
    if (!(row[0] > 0) || !(row[1] > 0) || !(row[2] > 0)) continue;
    liq.push_back({row[0], row[1], row[2], (uint8_t)(row[3] != 0)});
  }
  // Binance and Bybit streams arrive independently, so prints interleave in
  // arrival order; the panel reads the series as oldest → newest.
  std::sort(liq.begin(), liq.end(),
            [](const LiqPrint& a, const LiqPrint& b) { return a.ts < b.ts; });
  liqTop = base + n;
  ++version;
}

void MarketSeries::appendLiq(const double* data, int n, int64_t start, int symIdx) {
  if (!data || n <= 0) return;
  sym = symIdx;
  bool changed = false;
  size_t oldCount = liq.size();
  for (int i = 0; i < n; ++i) {
    if (start + i < liqTop) continue; // already known via the last full sync
    const double* row = data + (size_t)i * 4;
    if (!(row[0] > 0) || !(row[1] > 0) || !(row[2] > 0)) continue;
    liq.push_back({row[0], row[1], row[2], (uint8_t)(row[3] != 0)});
    changed = true;
  }
  liqTop = std::max(liqTop, start + n);
  if (changed) {
    // The retained prefix is already sorted and only the interleaved tail is
    // new — sort just the tail and splice instead of re-sorting everything.
    auto byTs = [](const LiqPrint& a, const LiqPrint& b) { return a.ts < b.ts; };
    std::sort(liq.begin() + (std::ptrdiff_t)oldCount, liq.end(), byTs);
    std::inplace_merge(liq.begin(), liq.begin() + (std::ptrdiff_t)oldCount,
                       liq.end(), byTs);
    if ((size_t)liq.size() > MAX_SAMPLES)
      liq.erase(liq.begin(), liq.end() - (std::ptrdiff_t)MAX_SAMPLES);
    ++version;
  }
}

void MarketSeries::clear() {
  liqTop = 0;
  oi.clear();
  funding.clear();
  liq.clear();
  sym = -1;
  ++version;
}
