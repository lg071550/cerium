#include "candles.h"

#include <cmath>

void CandleSeries::load(const double* data, int n, int interval, int symIdx) {
  intervalMin = interval;
  sym = symIdx;
  v.clear();
  v.reserve((size_t)n);
  for (int i = 0; i < n; ++i) {
    const double* k = data + (size_t)i * 7;
    double vol = k[5];
    double takerBuy = k[6];
    v.push_back({k[0], k[1], k[2], k[3], k[4], vol, 2.0 * takerBuy - vol});
  }
  if (v.size() > MAX_CANDLES) v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_CANDLES);
}

void CandleSeries::onTrade(double price, double qty, int side, double tsMs) {
  double intervalMs = (double)intervalMin * 60000.0;
  double bucket = std::floor(tsMs / intervalMs) * intervalMs;

  if (!v.empty()) {
    Candle& last = v.back();
    if (bucket == last.ts) {
      if (price > last.h) last.h = price;
      if (price < last.l) last.l = price;
      last.c = price;
      last.vol += qty;
      last.delta += side == 0 ? qty : -qty;
      return;
    }
    if (bucket < last.ts) return; // late trade for an old bucket — ignore
  }
  v.push_back({bucket, price, price, price, price, qty, side == 0 ? qty : -qty});
  if (v.size() > MAX_CANDLES) v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_CANDLES);
}
