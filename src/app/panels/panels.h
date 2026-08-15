#pragma once

// Shared state + entry points for the dock panels split out of terminal.cpp
// (orderbook, tape, feeds, watchlist). Each panel is a free function that
// receives the Ui, its content rect, and the per-panel view state Terminal
// owns — the same context the Terminal member draws used.

#include "../../data/feeds.h"
#include "../../ui/widgets.h"

#include <cstdint>
#include <vector>

// bounded cache: second-resolution timestamp → "HH:MM:SS" (direct-mapped)
struct TimeLabels {
  static constexpr int kSlots = 64;
  struct Slot {
    int64_t key = 0; // secs + 1; 0 = empty
    char text[12] = {};
  };
  Slot slots[kSlots];
};

// orderbook panel: view state + merged-book cache
// (ladder recomputed when version/filter/bin change)
struct OrderbookPanel {
  uint64_t mergeVersion = ~0ull;
  uint8_t mergeMask = 0xff;
  double mergeBin = -1;
  struct Level {
    double price, size, cum;
    bool ask;
    char priceLbl[24] = {}, sizeLbl[24] = {}; // formatted lazily, visible rows only
    double fmtP = -1.0, fmtS = -1.0;          // price/size as of last format
  };
  std::vector<Level> ladder; // descending price; cum from mid outward
  int ladderMid = 0;         // index of first bid in the ladder

  uint8_t mask = 7; // ClassSpot|ClassPerp|ClassDex
  double bin = 0;   // 0 = raw prices
  int scroll = 0;   // ladder rows scrolled away from mid
  int binSel = 0;   // index into the bin options
};

// tape panel: row list state + timestamp label cache
struct TapePanel {
  TimeLabels timeLabels;
  ListState list;
  const char* timeLabel(int64_t secs);
};

// feeds panel: venue list state
struct FeedsPanel {
  ListState list;
};

void drawOrderbook(Ui& u, Rect r, OrderbookPanel& st, Feeds& feeds);
void drawTape(Ui& u, Rect r, TapePanel& st, Feeds& feeds);
void drawFeeds(Ui& u, Rect r, FeedsPanel& st, Feeds& feeds);
void drawWatchlist(Ui& u, Rect r);
