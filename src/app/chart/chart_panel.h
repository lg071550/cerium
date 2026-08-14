#pragma once

#include "../../data/candles.h"
#include "chart_panes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Feeds;
struct Ui;

// Indicator registry entry. compute() fills `out` (sized to the series) over
// the FULL series; entries before minBars must be NaN. Runs only when the
// candle signature changes — drawing reads just the visible window.
struct Indicator {
  const char* name;
  bool overlay; // true: draws on the price pane; false: own pane below price
  int minBars;
  void (*compute)(const CandleSeries&, std::vector<float>& out);
};

const Indicator* indicatorRegistry();
int indicatorCount();

// right-click menu item, handed to the host (Terminal owns the menu overlay)
struct ChartMenuItem {
  std::string label;
  std::function<void()> action;
};

// The chart panel: header, price pane with overlays, stacked indicator panes,
// shared axis gutter, crosshair, scroll state, indicator toggles.
struct ChartPanel {
  int scroll = 0; // bars scrolled back from the latest

  void draw(Ui& u, Rect r, Feeds& feeds,
            const std::function<void(float, float, std::vector<ChartMenuItem>)>& openMenu);

private:
  std::vector<bool> m_enabled;             // per-registry-entry toggle
  std::vector<std::vector<float>> m_cache; // per-indicator full-series output
  std::vector<float> m_macdSignal;         // EMA9 of the cached MACD line
  uint64_t m_computedSig = ~0ull;          // full candle signature as of last compute
  uint64_t m_shapeSig = 0;                 // size/interval/symbol/last-bucket: a
                                           // change means history append/replace
  unsigned m_calcGen = 0;                  // bumped when toggles change → recompute
  unsigned m_calcToggles = 0;              // m_calcGen as of the last compute

  // O(1) live-tick state captured by the last full compute (values through
  // index n-2); lets a mutating last candle update just out[n-1]
  double m_cvdAcc = 0;      // CVD cumulative through n-2
  double m_rsiGain = 0, m_rsiLoss = 0; // Wilder state through n-2
  double m_e12 = 0, m_e26 = 0;         // MACD EMAs through n-2
  double m_sigEma = 0;                  // MACD signal EMA through n-2
  int m_sigValid = 0;                   // valid MACD bars seen through n-2
  bool m_liveState = false;             // tail states valid for updateLastBar

  // visible-window range cache: recomputed only when the window, candle
  // signature, or pane toggles change (history is append-only)
  struct PaneRange {
    bool ok;
    double lo, hi;
  };
  std::vector<PaneRange> m_rngPanes; // final pane ranges, top → bottom
  double m_rngPriceLo = 0, m_rngPriceHi = 1; // raw visible candle lo/hi
  uint64_t m_rngSig = 0;
  int m_rngV0 = -1, m_rngV1 = -1;
  unsigned m_rngGen = 0;     // bumped when indicator toggles change the panes
  unsigned m_rngToggles = 0; // m_rngGen as of the last range computation

  void ensureComputed(const CandleSeries& cs);
  void computeAll(const CandleSeries& cs); // full-series recompute (rare)
  bool updateLastBar(const CandleSeries& cs); // O(1)/O(period) live tick; false → full
};
