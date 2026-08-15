#pragma once

#include "../data/feeds.h"
#include "panels/panels.h"
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
  int debugLadderLevels() const { return (int)m_orderbook.ladder.size(); }

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

  // panel view state (draw functions live in panels/*.cpp)
  OrderbookPanel m_orderbook; // ladder cache + class/bin filters
  TapePanel m_tape;           // row list state + time-label cache
  FeedsPanel m_feedsPanel;    // venue list state

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
};
