#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Sorted flat-array L2 book side — a direct port of aggbook's BookSide
// (binary search + memmove insert/erase; cache-friendly).
struct BookSide {
  std::vector<double> prices;
  std::vector<double> sizes;

  size_t lowerBound(double price) const;
  void set(double price, double size); // size == 0 erases
  void loadSorted(const double* p, const double* s, size_t n); // ascending input
  void clear();

  size_t size() const { return prices.size(); }
  double bestHigh() const { return prices.empty() ? 0.0 : prices.back(); }
  double bestLow() const { return prices.empty() ? 0.0 : prices.front(); }
};

struct L2Book {
  BookSide bids, asks;
  uint64_t version = 0;

  void clear();
  double bestBid() const { return bids.bestHigh(); }
  double bestAsk() const { return asks.bestLow(); }
  double mid() const;
};
