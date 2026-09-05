#pragma once

#include "../../data/candles.h"

#include <array>
#include <cmath>
#include <cstddef>

// UTC calendar opens. Missing opening candles stay unknown: the left edge
// of a rolling history window is not a day/week/month open.
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
