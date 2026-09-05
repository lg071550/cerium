#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

inline std::optional<uint32_t> parseChartColor(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);
  if (text.size() != 3 && text.size() != 6) return std::nullopt;
  uint32_t rgb = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rgb, 16);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  if (text.size() == 3)
    rgb = ((rgb & 0xf00) << 8 | (rgb & 0x0f0) << 4 | (rgb & 0x00f)) * 17;
  return rgb;
}

inline int parseChartSettings(std::string_view text, std::span<int> values) {
  int count = 0;
  while (!text.empty() && (size_t)count < values.size()) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end == text.data()) return 0;
    values[(size_t)count++] = value;
    if (end == text.data() + text.size()) return count;
    if (*end != ',' || end + 1 == text.data() + text.size()) return 0;
    text.remove_prefix((size_t)(end + 1 - text.data()));
  }
  return 0;
}
