#pragma once

#include "../../ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

inline Color mixColor(Color a, Color b, float amount) {
  amount = std::clamp(amount, 0.0f, 1.0f);
  return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount,
          a.b + (b.b - a.b) * amount, a.a + (b.a - a.a) * amount};
}

// ---------------------------------------------------------------------------
// Formatting constants
// ---------------------------------------------------------------------------

constexpr int kAmountDecimals[] = {0, 2, 3, 5};
constexpr float kPanelRowHeights[] = {15.0f, 18.0f, 22.0f};
constexpr float kPanelIntensity[] = {0.65f, 1.0f, 1.35f};

// ---------------------------------------------------------------------------
// Number formatting
// ---------------------------------------------------------------------------

// Simple USD formatting (no precision control) — used by tape/liquidations.
inline void formatUsd(double value, char* out, size_t cap) {
  double v = std::fabs(value);
  if (v >= 1.0e9)
    snprintf(out, cap, "$%.2fB", v / 1.0e9);
  else if (v >= 1.0e6)
    snprintf(out, cap, "$%.2fM", v / 1.0e6);
  else if (v >= 1.0e3)
    snprintf(out, cap, "$%.1fK", v / 1.0e3);
  else
    snprintf(out, cap, "$%.2f", v);
}

// Full amount formatting with USD/base + precision — used by orderbook/dom.
inline void formatAmount(double value, bool usd, int precision, char* out,
                          size_t cap) {
  int decimals = kAmountDecimals[std::clamp(precision, 0, 3)];
  if (!usd) {
    snprintf(out, cap, "%.*f", decimals, value);
    return;
  }
  double v = std::fabs(value);
  int compactDecimals = std::clamp(decimals, 0, 2);
  if (v >= 1.0e9)
    snprintf(out, cap, "$%.*fB", compactDecimals, v / 1.0e9);
  else if (v >= 1.0e6)
    snprintf(out, cap, "$%.*fM", compactDecimals, v / 1.0e6);
  else if (v >= 1.0e3)
    snprintf(out, cap, "$%.*fK", compactDecimals, v / 1.0e3);
  else
    snprintf(out, cap, "$%.*f", compactDecimals, v);
}

inline void formatCoin(double value, int precision, char* out, size_t cap) {
  int decimals = kAmountDecimals[std::clamp(precision, 0, 3)];
  snprintf(out, cap, "%.*f", decimals, std::fabs(value));
}

inline void formatLot(double value, char* out, size_t cap) {
  double v = std::fabs(value);
  if (v >= 1000.0)
    snprintf(out, cap, "%.0f", v);
  else if (std::fabs(v - std::round(v)) < 0.08)
    snprintf(out, cap, "%.0f", std::round(v));
  else if (v >= 10.0)
    snprintf(out, cap, "%.0f", v);
  else
    snprintf(out, cap, "%.1f", v);
}

// ---------------------------------------------------------------------------
// USD parsing (for filter fields)
// ---------------------------------------------------------------------------

inline bool parseUsd(const std::string& input, double& value) {
  std::string clean;
  clean.reserve(input.size());
  for (char ch : input) {
    if (ch == '$' || ch == ',' || std::isspace((unsigned char)ch)) continue;
    clean.push_back((char)std::tolower((unsigned char)ch));
  }
  if (clean.empty()) {
    value = 0.0;
    return true;
  }

  double multiplier = 1.0;
  char suffix = clean.back();
  if (suffix == 'k' || suffix == 'm' || suffix == 'b') {
    clean.pop_back();
    multiplier = suffix == 'k' ? 1.0e3 : (suffix == 'm' ? 1.0e6 : 1.0e9);
  }
  if (clean.empty()) return false;

  char* end = nullptr;
  double parsed = std::strtod(clean.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(parsed) || parsed < 0.0)
    return false;
  value = parsed * multiplier;
  return std::isfinite(value);
}

inline void formatFilterValue(double value, std::string& out) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%.10g", value);
  out = buf;
}

// ---------------------------------------------------------------------------
// Depth/display scaling
// ---------------------------------------------------------------------------

inline float shapeDepth(float value, int mode) {
  value = std::clamp(value, 0.0f, 1.0f);
  if (mode == 1) return std::sqrt(value);
  if (mode == 2) return std::log1p(value * 9.0f) / std::log(10.0f);
  return value;
}
