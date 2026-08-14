#include "merge.h"

#include <algorithm>
#include <cmath>

void mergeSideWindow(const BookSide* const* sides, size_t count, double lo, double hi,
                     double binSize, bool asc, std::vector<MergedLevel>& out) {
  out.clear();
  if (lo >= hi) return;

  // Reused scratch, kept sorted by bin key. Sides are price-sorted ascending,
  // so each side's keys arrive non-decreasing and a single forward cursor bins
  // without per-call allocations (capacity persists across calls; the render
  // loop is single-threaded). Accumulation order matches the former
  // std::map-based version (side order, ascending price) so totals are
  // bit-identical.
  static std::vector<MergedLevel> bins;
  bins.clear();

  for (size_t s = 0; s < count; ++s) {
    const BookSide& side = *sides[s];
    size_t start = side.lowerBound(lo);
    size_t end = side.lowerBound(hi + 1e-12); // inclusive hi
    std::vector<MergedLevel>::iterator it = bins.begin();
    for (size_t i = start; i < end; ++i) {
      double key = binSize > 0 ? std::floor(side.prices[i] / binSize) * binSize
                               : side.prices[i];
      while (it != bins.end() && it->price < key) ++it;
      if (it != bins.end() && it->price == key) {
        it->size += side.sizes[i];
      } else {
        it = bins.insert(it, MergedLevel{key, side.sizes[i]});
      }
    }
  }

  out.reserve(out.size() + bins.size());
  if (asc) {
    for (const MergedLevel& lvl : bins) out.push_back(lvl);
  } else {
    for (auto it = bins.rbegin(); it != bins.rend(); ++it) out.push_back(*it);
  }
}
