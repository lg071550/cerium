#include "book.h"

#include <algorithm>

size_t BookSide::lowerBound(double price) const {
  return (size_t)(std::lower_bound(prices.begin(), prices.end(), price) - prices.begin());
}

void BookSide::set(double price, double size) {
  if (!(price > 0) || !(size >= 0)) return; // NaN/negative guard
  size_t i = lowerBound(price);
  if (i < prices.size() && prices[i] == price) {
    if (size == 0) {
      prices.erase(prices.begin() + (ptrdiff_t)i);
      sizes.erase(sizes.begin() + (ptrdiff_t)i);
    } else {
      sizes[i] = size;
    }
    return;
  }
  if (size == 0) return;
  prices.insert(prices.begin() + (ptrdiff_t)i, price);
  sizes.insert(sizes.begin() + (ptrdiff_t)i, size);
}

void BookSide::loadSorted(const double* p, const double* s, size_t n) {
  prices.assign(p, p + n);
  sizes.assign(s, s + n);
}

void BookSide::clear() {
  prices.clear();
  sizes.clear();
}

void L2Book::clear() {
  bids.clear();
  asks.clear();
  version++;
}

double L2Book::mid() const {
  double b = bestBid(), a = bestAsk();
  if (b <= 0 || a <= 0) return 0;
  return (a + b) * 0.5;
}
