#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>

// Shared price grouping / decimal policy for orderbook, DOM, tape, and the
// chart scale. The invariant: a displayed ladder never repeats the same
// rounded label on adjacent rows. AUTO grouping is a readable 1bp-ish step
// floored at the venue tick; AUTO decimals follow that step, and a coarser
// manual decimal count coarsens the step to match.

inline double nicePriceStep(double raw) {
  if (!(raw > 0) || !std::isfinite(raw)) return 0.01;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  if (!(mag > 0) || !std::isfinite(mag)) return 0.01;
  for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
    if (m * mag >= raw) return m * mag;
  return 10.0 * mag;
}

inline int priceDecimalsForStep(double step) {
  if (!(step > 0) || !std::isfinite(step)) return 2;
  int d = 0;
  while (d < 8) {
    double scaled = step * std::pow(10.0, d);
    if (std::fabs(scaled - std::round(scaled)) <= 1e-7) return d;
    ++d;
  }
  return 8;
}

inline int priceDecimalsForPrice(double price) {
  double a = std::fabs(price);
  if (!(a > 0) || !std::isfinite(a)) return 2;
  return priceDecimalsForStep(nicePriceStep(a * 1e-4));
}

inline double autoGroupStep(double mid, double nativeTick) {
  double readable = nicePriceStep(std::fabs(mid) * 1e-4);
  if (nativeTick > 0 && std::isfinite(nativeTick))
    return std::max(nativeTick, readable);
  return readable;
}

inline double coarsenStepToDecimals(double step, int decimals) {
  if (!(step > 0) || !std::isfinite(step)) return step;
  decimals = std::clamp(decimals, 0, 8);
  double grain = std::pow(10.0, -decimals);
  if (!(grain > 0) || !std::isfinite(grain)) return step;
  return step + grain * 1e-6 < grain ? grain : step;
}

// autoDec: follow the step. Otherwise honor userDec, coarsening the step
// when the user asked for fewer digits than the step can distinguish, and
// dropping padded zeros when they asked for more.
inline void resolvePriceDisplay(double& step, bool autoDec, int userDec,
                                int& outDec) {
  if (!(step > 0) || !std::isfinite(step)) step = 0.01;
  int natural = priceDecimalsForStep(step);
  if (autoDec) {
    outDec = natural;
    return;
  }
  userDec = std::clamp(userDec, 0, 8);
  if (userDec < natural) {
    step = coarsenStepToDecimals(step, userDec);
    outDec = userDec;
  } else {
    outDec = natural;
  }
}

inline void formatPrice(double price, int decimals, char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "%.*f", std::clamp(decimals, 0, 10), price);
}
