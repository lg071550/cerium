#pragma once

#include "../data/feeds.h"
#include "../data/merge.h"
#include "../dock/dock_drag.h"
#include "../dock/dock_tree.h"
#include "render/renderer.h"
#include "../ui/ui_context.h"
#include "../ui/widgets.h"
#include "chart/chart_panel.h"

#include <functional>
#include <string>
#include <vector>

struct PanelDef {
  int id;
  std::string title;
  std::function<void(Ui&, Rect)> draw;
};

// App root: top bar + dock workspace. Owns the panel registry, the dock tree,
// drag state, and per-panel widget state.
struct Terminal {
  Ui ui;
  DockTree dock;
  DockDrag drag;
  Feeds feeds;

  void init(Renderer* renderer);
  void frame(const Input& input, float dt, float cssW, float cssH);

  // perf: merged orderbook ladder size (diagnostics readout)
  int debugLadderLevels() const { return (int)m_ladder.size(); }

private:
  std::vector<PanelDef> m_panels;
  std::vector<std::string> m_titles; // panel id → title
  std::vector<std::pair<Rect, DockNode*>> m_splitters;
  DockNode* m_splitDrag = nullptr;
  std::vector<DockNode*> m_nodeStack; // per-frame dock traversal scratch (reused)
  const char* m_lastCursor = nullptr; // skip redundant shell_set_cursor calls
  Renderer* m_renderer = nullptr;
  float m_fps = 60.0f;
  bool m_showStats = false; // top-bar readout: fps ↔ frame stats (click to toggle)
  ListState m_feedsList;
  ListState m_tapeList;
  bool m_restored = false;
  float m_winW = 0, m_winH = 0;

  int addPanel(const char* title, std::function<void(Ui&, Rect)> fn);
  int panelId(const char* title) const;

  void buildDefaultLayout();
  void restoreLayout();
  void saveLayout();

  void drawTopBar(float w);
  void handleSplitters();
  void drawLeaf(DockNode* leaf);
  void drawTabStrip(DockNode* leaf, Rect strip);

  // panels
  void drawChart(Ui& u, Rect r);
  void drawOrderbook(Ui& u, Rect r);
  void drawTape(Ui& u, Rect r);
  void drawWatchlist(Ui& u, Rect r);
  void drawFeeds(Ui& u, Rect r);

  // merged-book cache (recomputed when version/filter/bin change)
  uint64_t m_mergeVersion = ~0ull;
  uint8_t m_mergeMask = 0xff;
  double m_mergeBin = -1;
  struct ObLevel {
    double price, size, cum;
    bool ask;
    char priceLbl[24] = {}, sizeLbl[24] = {}; // formatted lazily, visible rows only
    double fmtP = -1.0, fmtS = -1.0;          // price/size as of last format
  };
  std::vector<ObLevel> m_ladder;      // descending price; cum from mid outward
  int m_ladderMid = 0;                // index of first bid in the ladder

  // orderbook view state
  uint8_t m_obMask = 7;   // ClassSpot|ClassPerp|ClassDex
  double m_obBin = 0;     // 0 = raw prices
  int m_obScroll = 0;     // ladder rows scrolled away from mid
  int m_obBinSel = 0;     // index into the bin options

  // chart panel (view state + indicator toggles live in ChartPanel)
  ChartPanel m_chart;

  // context menu (overlay)
  struct MenuItem {
    std::string label;
    std::function<void()> action;
  };
  std::vector<MenuItem> m_menu;
  Rect m_menuRect{};
  uint64_t m_menuId = 0;
  void openMenu(float x, float y, std::vector<MenuItem> items, float winW, float winH);
  void drawMenu();
  void drawTooltip();

  // symbol picker (overlay)
  TextFieldState m_symSearch;
  uint64_t m_pickerId = 0;
  Rect m_pickerRect{};
  bool m_autoFocusPicker = false;
  void drawSymbolPicker();

  // bounded cache: second-resolution timestamp → "HH:MM:SS" (direct-mapped)
  struct TimeLabels {
    static constexpr int kSlots = 64;
    struct Slot {
      int64_t key = 0; // secs + 1; 0 = empty
      char text[12] = {};
    };
    Slot slots[kSlots];
  };
  TimeLabels m_timeLabels;
  const char* timeLabel(int64_t secs);
};
