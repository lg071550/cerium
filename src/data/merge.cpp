#include "merge.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Accumulates levels from multiple sorted book sides within [lo, hi] into
// per-price totals. binSize > 0 buckets prices into bins of that width.
// Output sorted ascending (asks) or descending (bids), truncated to the
// maxLevels bins nearest mid.
//
// Gather + sort + aggregate: the previous sorted-insert version shifted the
// bin vector on every cross-side insert (quadratic with 27 venues); a flat
// gather of each side's nearest-maxLevels levels plus one sort is linearithmic
// with tiny constants, and per-bin sums now accumulate in price order instead
// of side order (last-ulp differences only).
void mergeSideWindow(const BookSide* const* sides, size_t count, double lo, double hi,
                     double binSize, bool asc, std::vector<MergedLevel>& out,
                     size_t maxLevels) {
  out.clear();
  if (lo >= hi) return;

  struct KV {
    double key, size;
  };
  static std::vector<KV> flat; // reused scratch (single-threaded render loop)
  flat.clear();

  for (size_t s = 0; s < count; ++s) {
    const BookSide& side = *sides[s];
    size_t start = side.lowerBound(lo);
    size_t end = side.lowerBound(hi + 1e-12); // inclusive hi
    if (end - start > maxLevels) { // keep only the levels nearest mid
      if (asc) end = start + maxLevels; // asks: lowest prices
      else start = end - maxLevels;     // bids: highest prices
    }
    flat.reserve(flat.size() + (end - start));
    for (size_t i = start; i < end; ++i)
      flat.push_back(KV{binSize > 0 ? std::floor(side.prices[i] / binSize) * binSize
                                    : side.prices[i],
                        side.sizes[i]});
  }

  if (asc)
    std::sort(flat.begin(), flat.end(),
              [](const KV& a, const KV& b) { return a.key < b.key; });
  else
    std::sort(flat.begin(), flat.end(),
              [](const KV& a, const KV& b) { return a.key > b.key; });

  for (size_t i = 0; i < flat.size() && out.size() < maxLevels; ++i) {
    if (!out.empty() && out.back().price == flat[i].key)
      out.back().size += flat[i].size; // same bin: accumulate
    else
      out.push_back(MergedLevel{flat[i].key, flat[i].size});
  }
}