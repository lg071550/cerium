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
// descending (bids).
void mergeSideWindow(const BookSide* const* sides, size_t count, double lo, double hi,
                     double binSize, bool asc, std::vector<MergedLevel>& out);
