#pragma once

#include <cstddef>
#include <cstdint>

// Fixed-capacity trade ring, newest at the write head. 10k entries is ~320 KB.
struct TapeEntry {
  double price, qty, ts; // ts: exchange ms
  uint8_t venue, side;   // side: 0 = buy, 1 = sell
};

struct Tape {
  static constexpr size_t CAP = 10000;
  TapeEntry buf[CAP];
  size_t head = 0;  // next write slot
  size_t count = 0; // entries currently held

  void push(double price, double qty, double ts, uint8_t venue, uint8_t side) {
    TapeEntry& e = buf[head];
    e = {price, qty, ts, venue, side};
    head = (head + 1) % CAP;
    if (count < CAP) count++;
  }

  // Drop retained prints without touching the live feed. New trades begin
  // filling the ring again on the next bridge drain.
  void clear() {
    head = 0;
    count = 0;
  }

  // i = 0 → newest
  const TapeEntry* latest(size_t i) const {
    if (i >= count) return nullptr;
    size_t idx = (head + CAP - 1 - i) % CAP;
    return &buf[idx];
  }
};
