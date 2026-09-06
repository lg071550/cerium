#include "candles.h"

#include <cmath>

namespace {
bool holdForHistory(CandleSeries& s, double tsMs) {
  if (!s.awaitingHistory) return false;
  if (!(s.historyWaitT0 > 0)) s.historyWaitT0 = tsMs;
  if (tsMs - s.historyWaitT0 < 30000.0) return true;
  s.awaitingHistory = false; // REST never arrived; let live bars form
  return false;
}
} // namespace

void CandleSeries::load(const double* data, int n, Timeframe tf_, int symIdx, bool preserveLive) {
  std::vector<Candle> liveTail;
  if (preserveLive && tf == tf_ && sym == symIdx && tf.kind == Timeframe::Time && data && n > 0) {
    const double lastTs = data[(size_t)(n - 1) * 7];
    for (const auto& c : v) if (c.ts >= lastTs) liveTail.push_back(c);
  }
  ++historyVersion;
  tf = tf_;
  sym = symIdx;
  // Treat the loaded tail tick bar as already closed so the first live trade
  // opens a fresh one (its count is unknown here). Volume bars keep the final
  // (partial) bar open and continue it with aggregated live volume in onAgg.
  barCount = tf.kind == Timeframe::Tick ? (int)tf.value : 0;
  v.clear();
  n = data && n > 0 ? n : 0;
  const int first = n > (int)MAX_CANDLES ? n - (int)MAX_CANDLES : 0;
  v.reserve((size_t)(n - first));
  for (int i = first; i < n; ++i) {
    const double* k = data + (size_t)i * 7;
    bool finite = true;
    for (int j = 0; j < 7; ++j) finite = finite && std::isfinite(k[j]);
    if (!finite || !(k[0] > 0) || !(k[3] > 0) || k[2] < k[3] ||
        k[1] < k[3] || k[1] > k[2] || k[4] < k[3] || k[4] > k[2] ||
        k[5] < 0 || k[6] < 0 || k[6] > k[5]) continue;
    if (!v.empty() && k[0] < v.back().ts) continue;
    double vol = k[5];
    double takerBuy = k[6];
    // Historical seed is Binance-only: aggregated volume/delta start from the
    // same reference and diverge only as live prints from other venues fold in.
    v.push_back({k[0], k[1], k[2], k[3], k[4], vol, 2.0 * takerBuy - vol, vol});
  }
  for (const auto& c : liveTail) {
    if (!v.empty() && v.back().ts == c.ts) v.back() = c;
    else if (v.empty() || c.ts > v.back().ts) v.push_back(c);
  }
  if (v.size() > MAX_CANDLES) v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_CANDLES);
  awaitingHistory = false;
  historyLoaded = true;
  historyWaitT0 = 0;
}

void CandleSeries::onTrade(double price, double qty, int side, double tsMs) {
  if (!(price > 0) || !(qty > 0) || !(tsMs > 0) ||
      !std::isfinite(price) || !std::isfinite(qty) || !std::isfinite(tsMs)) return;
  if (holdForHistory(*this, tsMs)) return;
  if (tf.kind == Timeframe::Volume) return; // volume bars are aggregated in onAgg
  double bucket = tsMs; // tick bars timestamp by their first trade
  if (tf.kind == Timeframe::Time)
    bucket = std::floor(tsMs / (tf.value * 60000.0)) * (tf.value * 60000.0);
  if (!v.empty()) {
    Candle& last = v.back();
    bool same;
    if (tf.kind == Timeframe::Time) {
      if (bucket < last.ts) return; // late trade for an old bucket — ignore
      same = bucket == last.ts;
    } else { // Tick
      same = barCount < (int)tf.value;
    }
    if (same) {
      if (price > last.h) last.h = price;
      if (price < last.l) last.l = price;
      last.c = price;
      last.vol += qty;
      if (tf.kind == Timeframe::Tick) ++barCount;
      return;
    }
  }
  v.push_back({bucket, price, price, price, price, qty, 0.0, 0.0});
  barCount = 1;
  if (v.size() > MAX_CANDLES) v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_CANDLES);
}

// Aggregated flow: fold mask-filtered venue prints into the forming bar's
// total volume and CVD delta. For time/tick bars the boundary is venue-0
// driven (onTrade) and this only accumulates aggVol/delta; for volume bars
// this owns the boundary too — the bar closes when aggregated volume reaches
// the target, and its OHLC reflects every mask venue's prints.
void CandleSeries::onAgg(double price, double qty, int side, double tsMs) {
  if (!(price > 0) || !(qty > 0) || !(tsMs > 0) ||
      !std::isfinite(price) || !std::isfinite(qty) || !std::isfinite(tsMs)) return;
  if (holdForHistory(*this, tsMs)) return;

  if (tf.kind == Timeframe::Volume) {
    if (v.empty() || v.back().aggVol >= tf.value)
      v.push_back({tsMs, price, price, price, price, 0.0, 0.0, 0.0});
    Candle& last = v.back();
    if (price > last.h) last.h = price;
    if (price < last.l) last.l = price;
    last.c = price;
    last.aggVol += qty;
    last.delta += side == 0 ? qty : -qty;
    if (v.size() > MAX_CANDLES)
      v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_CANDLES);
    return;
  }

  if (v.empty()) return;
  Candle& last = v.back();
  if (tf.kind == Timeframe::Time) {
    double intervalMs = tf.value * 60000.0;
    double bucket = std::floor(tsMs / intervalMs) * intervalMs;
    if (bucket < last.ts) return; // late print for a closed bucket — ignore
  }
  last.aggVol += qty;
  last.delta += side == 0 ? qty : -qty;
}
