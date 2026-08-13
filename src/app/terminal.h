#pragma once

#include "../data/feeds.h"
#include "../dock/dock_drag.h"
#include "../dock/dock_tree.h"
#include "../ui/ui_context.h"
#include "../ui/widgets.h"

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

  void init(GlyphAtlas* atlas);
  void frame(const Input& input, float dt, float cssW, float cssH);

private:
  std::vector<PanelDef> m_panels;
  std::vector<std::string> m_titles; // panel id → title
  std::vector<std::pair<Rect, DockNode*>> m_splitters;
  DockNode* m_splitDrag = nullptr;
  float m_fps = 60.0f;
  bool m_restored = false;

  int addPanel(const char* title, std::function<void(Ui&, Rect)> fn);
  int panelId(const char* title) const;

  void buildDefaultLayout();
  void restoreLayout();
  void saveLayout();

  void drawTopBar(float w);
  void handleSplitters();
  void drawLeaf(DockNode* leaf);
  void drawTabStrip(DockNode* leaf, Rect strip);

  // demo panels
  void drawChart(Ui& u, Rect r);
  void drawOrderbook(Ui& u, Rect r);
  void drawTape(Ui& u, Rect r);
  void drawWatchlist(Ui& u, Rect r);
};
