#pragma once

#include <algorithm>
#include "chart_panes.h"
#include "../../data/ht_map.h"

// Draw-side caches belong to a chart: adjacent views may use different scales,
// windows, or book samples even when their timestamps happen to match.
struct HtProfileCache {
  uint64_t version = 0;
  bool valid = false;
  ChartPane pane{};
  std::vector<float> longs, shorts;

  bool update(const HtLayer& layer, uint64_t revision, const ChartPane& next) {
    if (valid && revision == version && pane.lo == next.lo && pane.hi == next.hi &&
        pane.log == next.log && pane.area.y == next.area.y && pane.area.h == next.area.h)
      return false;
    valid = true; version = revision; pane = next;
    const int first = (int)std::floor(pane.area.y / 2.0f);
    const int last = (int)std::floor((pane.area.y + pane.area.h - .01f) / 2.0f);
    longs.assign((size_t)std::max(0, last - first + 1), 0);
    shorts.assign(longs.size(), 0);
    for (const auto& band : layer.bands) {
      if (band.hi <= pane.lo || band.lo >= pane.hi) continue;
      float top = pane.yOf(band.hi), bottom = pane.yOf(band.lo);
      if (!std::isfinite(top) || !std::isfinite(bottom)) continue;
      if (bottom < top) std::swap(top, bottom);
      top = std::max(top, pane.area.y);
      bottom = std::min(bottom, pane.area.y + pane.area.h);
      const int a = std::max(first, (int)std::floor(top / 2.0f));
      const int b = std::min(last, (int)std::floor((bottom - .001f) / 2.0f));
      for (int row = a; row <= b; ++row) {
        longs[(size_t)(row-first)] = std::max(longs[(size_t)(row-first)], band.longUsd);
        shorts[(size_t)(row-first)] = std::max(shorts[(size_t)(row-first)], band.shortUsd);
      }
    }
    return true;
  }
};

struct VolumeProfileCache {
  struct Key {
    int mode = 0, a0 = 0, a1 = 0, rows = 0;
    int64_t r0 = 0;
    double bin = 0;
    uint64_t ofKey = 0, csKey = 0;
    bool operator==(const Key&) const = default;
  };
  Key key{};
  bool valid = false;
  std::vector<double> buy, sell;
};

struct BookHeatCache {
  struct Column {
    int64_t ts = 0;
    uint64_t mut = 0;
    std::vector<int64_t> rows;
    std::vector<float> heat;
  };
  std::vector<Column> columns;
  size_t total = 0;
};
