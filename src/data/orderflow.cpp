#include "orderflow.h"

#include <algorithm>
#include <cmath>

void OrderFlowSeries::load(const double* data, int n, Timeframe tf_, int symIdx,
                           bool prepend) {
  tf = tf_;
  sym = symIdx;
  n = std::max(0, n);
  std::vector<OrderFlowTrade> in;
  in.reserve((size_t)n);
  for (int i = 0; i < n; ++i) {
    const double* row = data + (size_t)i * 4;
    if (!(row[0] > 0) || !(row[1] > 0) || !(row[2] > 0) ||
        !std::isfinite(row[0]) || !std::isfinite(row[1]) || !std::isfinite(row[2]) ||
        (row[3] != 0 && row[3] != 1)) continue;
    in.push_back({row[0], row[1], row[2], (uint8_t)(row[3] != 0)});
  }
  if (in.empty()) return;

  if (prepend && !v.empty()) {
    // Worker prepends disjoint ID ranges. Distinct trades at the seam can
    // share a millisecond timestamp; strict timestamp comparison lost them.
    double front = v.front().ts;
    size_t keep = 0;
    while (keep < in.size() && in[keep].ts <= front) ++keep;
    if (keep == 0) return;
    size_t room = v.size() < MAX_TRADES ? MAX_TRADES - v.size() : 0;
    if (room == 0) return;
    size_t take = std::min(keep, room);
    // Keep the suffix of the older page so it stays contiguous with v.front().
    std::vector<OrderFlowTrade> next;
    next.reserve(take + v.size());
    next.insert(next.end(), in.begin() + (ptrdiff_t)(keep - take),
                in.begin() + (ptrdiff_t)keep);
    next.insert(next.end(), v.begin(), v.end());
    v.swap(next);
    indexShift += (int64_t)take;
    ++prepends;
    ++generation;
    ++version;
    return;
  }

  int first = std::max(0, (int)in.size() - (int)MAX_TRADES);
  double newest = in.back().ts;
  std::vector<OrderFlowTrade> tail;
  for (const OrderFlowTrade& t : v)
    if (t.ts > newest) tail.push_back(t);
  v.clear();
  v.reserve((size_t)(in.size() - (size_t)first) + tail.size());
  v.insert(v.end(), in.begin() + first, in.end());
  v.insert(v.end(), tail.begin(), tail.end());
  if (v.size() > MAX_TRADES)
    v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_TRADES);
  indexShift = 0;
  ++generation;
  ++version;
}

void OrderFlowSeries::onTrade(double price, double qty, int side, double tsMs) {
  if (!(price > 0) || !(qty > 0) || !(tsMs > 0)) return;
  // A bootstrap may overlap prints already received from the websocket. Exact
  // timestamp/price/qty/side matches at the tail are safe to discard.
  if (!v.empty()) {
    const OrderFlowTrade& last = v.back();
    if (last.ts == tsMs && last.price == price && last.qty == qty &&
        last.side == (uint8_t)(side != 0))
      return;
  }
  v.push_back({tsMs, price, qty, (uint8_t)(side != 0)});
  // Compact in chunks instead of shifting the retained window on every print.
  // `generation` tells chart caches that their source indices changed.
  if (v.size() > MAX_TRADES + 1024) {
    const size_t drop = v.size() - MAX_TRADES;
    v.erase(v.begin(), v.end() - (ptrdiff_t)MAX_TRADES);
    indexShift -= (int64_t)drop;
    ++generation;
  }
  ++version;
}

void OrderFlowSeries::clear() {
  v.clear();
  sym = -1;
  indexShift = 0;
  ++generation;
  ++version;
  prepends = 0;
}
