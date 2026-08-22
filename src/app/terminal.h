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
#include <memory>
#include <string>
#include <vector>

enum class PanelKind { Chart, Orderbook, Dom, Tape, Liquidations, Watchlist, Feeds };

struct PanelDef {
  int id;
  PanelKind kind;
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
  void frame(const Input& input, float frameDt, float uiDt, float cssW, float cssH);

  // perf: merged orderbook ladder size (diagnostics readout)
  int debugLadderLevels() const;

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
  bool m_showTabClose = true;
  bool m_showTabStrips = true;
  bool m_restored = false;
  float m_winW = 0, m_winH = 0;
  float m_layoutDirtyAt = -10.0f; // ui.time of the latest dock change (debounced persistence)

  int addPanel(PanelKind kind, const std::string& title = {});
  int addWidget(PanelKind kind, DockNode* host);
  void ensurePanelsForLayout(const char* json);
  int panelId(const char* title) const;

  void buildDefaultLayout();
  void restoreLayout();
  void saveLayout();

  void drawTopBar(float w);
  void handleSplitters();
  void drawLeaf(DockNode* leaf);
  void drawTabStrip(DockNode* leaf, Rect strip);

  // Per-widget state lives on stable heap addresses so adding another panel
  // cannot invalidate draw callbacks already registered in m_panels.
  std::vector<std::unique_ptr<OrderbookPanel>> m_orderbooks;
  std::vector<std::unique_ptr<DomPanel>> m_doms;
  std::vector<std::unique_ptr<TapePanel>> m_tapes;
  std::vector<std::unique_ptr<LiquidationsPanel>> m_liquidations;
  std::vector<std::unique_ptr<FeedsPanel>> m_feedPanels;
  std::vector<std::unique_ptr<ChartPanel>> m_charts;

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

  // add-widget popup (overlay)
  uint64_t m_widgetsId = 0;
  Rect m_widgetsRect{};
  void drawWidgetsPicker();

  // named layout snapshots: working copy stays in cerium.layout.v1; snapshots
  // live at cerium.layouts.<name>, indexed by cerium.layouts.index (JSON array
  // of {"name","fav"}). Implementation in layouts.cpp.
  struct SavedLayout {
    std::string name;
    bool favorite = false;
  };
  std::vector<SavedLayout> m_layouts;
  bool m_layoutsLoaded = false;
  std::string m_activeLayout;   // snapshot the working copy came from (may be "")
  std::string m_layoutError;    // save/rename error line (empty = none)
  uint64_t m_layoutsId = 0;
  Rect m_layoutsRect{};
  TextFieldState m_layoutName;  // "save current" field
  int m_renaming = -1;          // m_layouts index being renamed inline
  TextFieldState m_renameInput;
  bool m_renameFresh = false;

  void drawLayoutsMenu();
  void loadLayoutIndex();
  void saveLayoutIndex();
  void sortLayouts(); // favorites first, stable
  bool layoutNameTaken(const std::string& name, int skip = -1) const;
  void saveLayoutAs(const std::string& name);
  void switchToLayout(const std::string& name);
  void renameLayout(int i, const std::string& name);
  void duplicateLayout(int i);
  void deleteLayout(int i);
};
