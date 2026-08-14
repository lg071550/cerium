#pragma once

#include "book.h"

#include <cstddef>
#include <vector>

struct MergedLevel {
  double price;
  double size;
};

// Accumulates levels from multiple sorted book sides within [lo, hi] into
// per-price totals — port of aggbook's mergeSideWindow. binSize > 0 buckets
// prices into bins of that width. Output sorted ascending (asks) or
// descending (bids). maxLevels caps each side's contribution to the levels
// nearest mid (lowest asks / highest bids) so the merged ladder stays bounded
// regardless of book depth — the nearest-maxLevels merged bins only ever draw
// from each side's nearest-maxLevels levels.
void mergeSideWindow(const BookSide* const* sides, size_t count, double lo, double hi,
                     double binSize, bool asc, std::vector<MergedLevel>& out,
                     size_t maxLevels = SIZE_MAX);
