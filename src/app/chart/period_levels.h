#pragma once

#include "../../data/candles.h"

#include <array>
#include <cmath>
#include <cstddef>

// Exact daily metadata supplies UTC opens on custom timeframes. Without it,
// only an exact boundary candle qualifies; missing opens remain unknown.
struct PeriodOpen {
  double price = NAN;
  double startMs = 0;
  int bar = -1;
  bool known() const { return bar >= 0 && std::isfinite(price); }
};

struct PeriodLevels {
  enum Period : size_t { Day, Week, Month, Count };
  std::array<PeriodOpen, Count> current;
  std::array<PeriodOpen, Count> previous;
};

PeriodLevels computePeriodLevels(const CandleSeries& candles);
