#pragma once

// Shared state + entry points for the dock panels split out of terminal.cpp
// (orderbook, tape, feeds, watchlist). Each panel is a free function that
// receives the Ui, its content rect, and the per-panel view state Terminal
// owns — the same context the Terminal member draws used.

#include "../../data/feeds.h"
#include "../../data/dom_ladder.h"
#include "../../ui/widgets.h"

#include <cstdint>
#include <string>
#include <vector>

// bounded cache: second-resolution timestamp → "HH:MM:SS" (direct-mapped).
// Tape rows re-render the same entries every frame; the cache keeps
// localtime_r + snprintf off that path.
struct TimeLabels {
  static constexpr int kSlots = 64;
  struct Slot {
    int64_t key = 0; // secs + 1; 0 = empty
    char text[12] = {};
  };
  Slot slots[kSlots];

  const char* label(int64_t secs) {
    Slot& s = slots[(size_t)(((uint64_t)secs * 0x9E3779B97F4A7C15ull) >> 58)];
    if (s.key != secs + 1) {
      time_t t = (time_t)secs;
      struct tm tmv;
      localtime_r(&t, &tmv);
      s.key = secs + 1;
      snprintf(s.text, sizeof(s.text), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min,
               tmv.tm_sec);
    }
    return s.text;
  }
};

// orderbook panel: view state + merged-book cache
// (ladder recomputed when version/filter/bin change)
struct OrderbookPanel {
  std::string settingsKey = "cerium.orderbook.settings.v1";
  uint64_t mergeVersion = ~0ull;
  int mergeCap = 0; // viewable-depth merge cap; changes rebuild now
  uint32_t mergeMask = 0;
  double mergeBin = -1;
  struct Level {
    double price, size, cum, cumUsd;
    bool ask;
    char priceLbl[24] = {}, sizeLbl[24] = {}; // formatted lazily, visible rows only
    double fmtP = -1.0, fmtS = -1.0;          // price/size as of last format
  };
  std::vector<Level> ladder; // descending price; cum from mid outward
  int ladderMid = 0;         // index of first bid in the ladder

  // Compact, rebuild-driven orderbook analytics. Values are USD notional so
  // cross-asset comparisons remain meaningful even when ladder amounts are
  // displayed in base units. Bands are +/-0.5%, 1%, 2.5%, 5%, and 10% from
  // aggMid (band order matches kDepthBands in orderbook.cpp).
  struct Analytics {
    static constexpr int kBands = 5;
    double bidUsd[kBands] = {}, askUsd[kBands] = {};
    double imbalance[kBands] = {}; // (bid - ask) / (bid + ask), -1..1
    double bestBid = 0, bestAsk = 0;
    double weightedMid = 0, spreadBps = 0;
    bool valid = false;
  } analytics;
  char analyticsTip[256] = {};

  uint32_t mask = kAllVenuesMask; // venues feeding the merged book
  double bin = 0;   // 0 = raw prices
  int scroll = 0;   // ladder rows scrolled away from mid
  int binSel = 0;   // index into the bin options

  uint64_t flowPickerId = 0;
  Rect flowPickerRect{};
  ListState flowPickerList;

  // Widget-local presentation settings. Source mask and price bin remain in
  // the compact toolbar; the complete set is also available on the settings
  // page and persisted together.
  bool settingsOpen = false;
  bool settingsLoaded = false;
  bool showUsd = false;
  bool showCumulative = false;
  bool showDepth = true;
  bool showBars = true;
  bool showGradient = true;
  bool showTexture = true;
  bool showEdges = true;
  bool showFeedCount = true;
  bool showAnalytics = true;
  bool showDepthBands = true;
  bool showWeightedMid = true;
  int density = 1;       // tight / normal / relaxed
  int levelLimit = 0;    // auto / 20 / 40 / 60 / 100
  int scaleMode = 0;     // linear / square-root / logarithmic
  int intensity = 1;     // quiet / normal / strong
  int priceDecimals = 2;
  int amountPrecision = 2; // option index: 0 / 2 / 3 / 5 decimals
  int depthWidth = 1;      // 42 / 58 / 74 percent
  int barWidth = 1;        // 24 / 34 / 46 percent
  int sideMode = 0;        // both / asks / bids
  ListState settingsList;

  void drawFlowPicker(Ui& u, Feeds& feeds);
};

// tape panel: row list state + timestamp label cache
struct TapePanel {
  std::string settingsKey = "cerium.tape.settings.v1";
  TimeLabels timeLabels;
  ListState list;

  // Widget-local presentation and filter settings. Filters are expressed in
  // USD notional regardless of the selected display unit, so their meaning is
  // stable when switching between USD and base-asset quantity.
  bool settingsOpen = false;
  bool showUsd = true;
  bool settingsLoaded = false;
  bool showTime = true;
  bool showVenue = true;
  bool showWash = true;
  bool showGradient = true;
  bool showMarker = true;
  int density = 1;       // tight / normal / relaxed
  int intensity = 1;     // quiet / normal / strong
  int priceDecimals = 2;
  int amountPrecision = 2; // option index: 0 / 2 / 3 / 5 decimals
  double minUsd = 0.0;
  double maxUsd = 0.0; // zero = no upper bound
  TextFieldState minInput;
  TextFieldState maxInput;
  std::string filterError;
  ListState settingsList;

  // Newest-first indices into Tape, rebuilt only when the ring or filter
  // changes. This keeps list virtualization intact after filtering.
  std::vector<size_t> filtered;
  size_t filterHead = static_cast<size_t>(-1);
  size_t filterCount = static_cast<size_t>(-1);
  double filterMin = -1.0;
  double filterMax = -1.0;

  const char* timeLabel(int64_t secs);
};

// liquidations panel: aggregated force-order list + size filter state
struct LiquidationsPanel {
  std::string settingsKey = "cerium.liquidations.settings.v1";
  TimeLabels timeLabels;
  ListState list;
  ListState settingsList;

  bool settingsOpen = false;
  bool settingsLoaded = false;
  bool showUsd = true;
  bool showTime = true;
  bool showNoise = true;
  bool showGradient = true;
  bool showMarker = true;
  int sideMode = 0;      // all / shorts liquidated / longs liquidated
  int density = 1;       // tight / normal / relaxed
  int intensity = 1;     // quiet / normal / strong
  int amountPrecision = 2;
  double minUsd = 0.0;
  double maxUsd = 0.0;   // zero = no upper bound
  TextFieldState minInput;
  TextFieldState maxInput;
  std::string filterError;

  std::vector<size_t> filtered; // newest-first indices into MarketSeries::liq
  uint64_t filterVersion = ~0ull;
  double filterMin = -1.0;
  double filterMax = -1.0;
  int filterSide = -1;

  const char* timeLabel(int64_t secs);
};

// feeds panel: venue list state
struct FeedsPanel {
  ListState list;
};

struct DomPanel {
  std::string settingsKey = "cerium.dom.settings.v1";
  DomModel model;
  int venue = 0; // -1 = normalized cross-venue analytics; 0 = Binance Perp default
  uint32_t mask = kAllVenuesMask;
  // (no clock throttle: the model rebuilds whenever the source books move)
  bool settingsOpen = false, settingsLoaded = false;
  bool showUsd = false, showTrades = true, showCumulative = true;
  bool showVenueCounts = true, showSummary = true, showInspector = true;
  bool showBars = true, showTexture = false, showQueue = true, showEdges = true;
  bool showWalls = true, showFlowFlashes = true;
  int density = 1;       // tight / normal / relaxed
  int levelLimit = 0;    // auto / 40 / 80 / 160
  int tradeWindow = 1;   // 5 / 15 / 60 seconds
  int groupMode = 0;     // auto / 1x / 2x / 5x / 10x / custom
  int scaleMode = 0;     // linear / sqrt / log
  int intensity = 1;     // quiet / normal / strong
  int amountPrecision = 2; // 0 / 2 / 3 / 5 decimals
  int pricePrecision = 2;  // auto / 1 / 2 / 3 / 4
  double customStep = 1.0;
  TextFieldState customStepInput;
  std::string customStepError;
  ListState settingsList;

  bool autoCenter = true;
  int64_t centerOffset = 0;
  int64_t pinnedTick = 0, hoverTick = 0;
  double pinnedPrice = 0;
  bool hasPinned = false, hasHover = false;
  int pinnedSymbol = 0, pinnedVenue = -1;
  uint64_t tickVersion = ~0ull;
  int tickVenue = -2;
  uint32_t tickMask = 0;
  double inferredStep = 0, effectiveStep = 0;

  uint64_t venuePickerId = 0;
  Rect venuePickerRect{};
  TextFieldState venueSearch;
  ListState venueList;
  std::vector<int> filteredVenues;
  bool autoFocusVenue = false;

  uint64_t flowPickerId = 0;
  Rect flowPickerRect{};
  ListState flowPickerList;

  void drawVenuePicker(Ui& u, Feeds& feeds);
  void drawFlowPicker(Ui& u, Feeds& feeds);
};

void drawOrderbook(Ui& u, Rect r, OrderbookPanel& st, Feeds& feeds);
void drawDom(Ui& u, Rect r, DomPanel& st, Feeds& feeds);
void drawTape(Ui& u, Rect r, TapePanel& st, Feeds& feeds);
void drawLiquidations(Ui& u, Rect r, LiquidationsPanel& st, Feeds& feeds);
void drawFeeds(Ui& u, Rect r, FeedsPanel& st, Feeds& feeds);
void drawWatchlist(Ui& u, Rect r, Feeds& feeds);
