#include "terminal.h"

#include "../dock/dock_layout.h"
#include "../platform/shell.h"
#include "../ui/theme.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// 1s rolling frame-cost window maintained by main.cpp (index → metric)
extern "C" double cerium_perf(int i);

static const char* kLayoutKey = "cerium.layout.v1";

// canonical symbols — index matches feeds/registry.ts SYMBOLS
static const char* kSyms[] = {"ETH", "BTC", "SOL"};

// compact count for the stats readout: 950 → "950", 3200 → "3.2k", 41000 → "41k"
static void fmtCount(char* out, size_t n, int v) {
  if (v >= 10000) snprintf(out, n, "%dk", v / 1000);
  else if (v >= 1000) snprintf(out, n, "%.1fk", v / 1000.0);
  else snprintf(out, n, "%d", v);
}

int Terminal::addPanel(const char* title, std::function<void(Ui&, Rect)> fn) {
  int id = (int)m_panels.size();
  m_panels.push_back({id, title, std::move(fn)});
  m_titles.push_back(title);
  return id;
}

int Terminal::panelId(const char* title) const {
  for (size_t i = 0; i < m_titles.size(); ++i)
    if (m_titles[i] == title) return (int)i;
  return -1;
}

void Terminal::init(Renderer* renderer) {
  m_renderer = renderer;
  ui.init(renderer->atlas());
  ui.draw.setTextShadowColor(theme().textShadow);

  addPanel("Chart", [this](Ui& u, Rect r) { drawChart(u, r); });
  addPanel("Orderbook",
           [this](Ui& u, Rect r) { drawOrderbook(u, r, m_orderbook, feeds); });
  addPanel("Tape", [this](Ui& u, Rect r) { drawTape(u, r, m_tape, feeds); });
  addPanel("Watchlist", [](Ui& u, Rect r) { drawWatchlist(u, r); });
  addPanel("Feeds",
           [this](Ui& u, Rect r) { drawFeeds(u, r, m_feedsPanel, feeds); });

  buildDefaultLayout();
  feeds.init();

  if (char* sc = shell_storage_get("cerium.uiscale")) {
    float s = (float)atof(sc);
    if (s >= 0.5f && s <= 2.5f) themeApplyScale(s);
    free(sc);
  }
}

void Terminal::buildDefaultLayout() {
  DockNode* chart = dock.makeLeaf({panelId("Chart")});
  DockNode* ob = dock.makeLeaf({panelId("Orderbook")});
  DockNode* tape = dock.makeLeaf({panelId("Tape"), panelId("Watchlist"), panelId("Feeds")});
  DockNode* right = dock.makeSplit(DockDir::Vertical, ob, tape, 0.5f);
  dock.setRoot(dock.makeSplit(DockDir::Horizontal, chart, right, 0.66f));
}

void Terminal::restoreLayout() {
  char* json = shell_storage_get(kLayoutKey);
  if (json) {
    if (dockDeserialize(dock, json, m_titles)) {
      free(json);
      // panels added since the layout was saved must still appear
      std::vector<int> present;
      dock.collectTabs(present);
      DockNode* host = dock.firstLeaf();
      for (size_t pid = 0; pid < m_panels.size(); ++pid) {
        bool found = std::find(present.begin(), present.end(), (int)pid) != present.end();
        if (!found && host) dock.insertTab(host, (int)pid);
      }
      return;
    }
    free(json);
  }
  // keep the default layout built in init()
}

void Terminal::saveLayout() {
  std::string json = dockSerialize(dock, m_titles);
  if (!json.empty()) shell_storage_set(kLayoutKey, json.c_str());
}

void Terminal::frame(const Input& input, float dt, float cssW, float cssH) {
  if (!m_restored) {
    restoreLayout();
    m_restored = true;
  }
  if (dt > 0) m_fps = m_fps * 0.95f + (1.0f / dt) * 0.05f;

  ui.begin(input, dt);
  ui.overlayGate();
  const Theme& t = theme();

  m_winW = cssW;
  m_winH = cssH;

  // UI zoom: Ctrl+= / Ctrl+- / Ctrl+0
  for (int i = 0; i < input.keyCount; ++i) {
    const KeyEvent& k = input.keys[i];
    if (!k.down || !k.ctrl) continue;
    float s = themeScale();
    if (k.keyCode == 187 || k.keyCode == 61) s += 0.1f;       // '='
    else if (k.keyCode == 189 || k.keyCode == 173) s -= 0.1f; // '-'
    else if (k.keyCode == 48) s = 1.0f;                       // '0'
    else continue;
    s = std::clamp(s, 0.75f, 2.0f);
    themeApplyScale(s);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)s);
    shell_storage_set("cerium.uiscale", buf);
  }

  drawTopBar(cssW);

  Rect area{0, t.topBarH, cssW, cssH - t.topBarH};
  dock.computeRects(area, t.splitterSize);

  if (drag.armed || drag.active) {
    drag.update(ui, dock);
  } else {
    handleSplitters();
  }

  // panels
  m_nodeStack.clear();
  m_nodeStack.push_back(dock.root);
  while (!m_nodeStack.empty()) {
    DockNode* n = m_nodeStack.back();
    m_nodeStack.pop_back();
    if (!n) continue;
    if (n->isLeaf()) drawLeaf(n);
    else {
      m_nodeStack.push_back(n->a);
      m_nodeStack.push_back(n->b);
    }
  }

  // splitter visuals on top of panel borders (+ resize cursor)
  const char* cursor = "default";
  if (drag.active) {
    cursor = "grabbing";
  } else if (m_splitDrag) {
    cursor = m_splitDrag->dir == DockDir::Horizontal ? "col-resize" : "row-resize";
  } else {
    m_splitters.clear();
    dock.collectSplitters(t.splitterSize, m_splitters);
    for (auto& [sr, node] : m_splitters) {
      if (ui.hovered(sr)) {
        // subtle 2px accent bar centered in the gap
        Rect bar = node->dir == DockDir::Horizontal
                       ? Rect{sr.x + (sr.w - 2) * 0.5f, sr.y, 2, sr.h}
                       : Rect{sr.x, sr.y + (sr.h - 2) * 0.5f, sr.w, 2};
        ui.draw.rect(bar, withAlpha(t.accent, 0.5f), 1.0f);
        cursor = node->dir == DockDir::Horizontal ? "col-resize" : "row-resize";
      }
    }
    if (cursor[0] == 'd' && ui.hot) cursor = "pointer";
  }
  if (cursor != m_lastCursor) { // all values are literals — pointer compare
    shell_set_cursor(cursor);
    m_lastCursor = cursor;
  }

  ui.inOverlayPass = true;
  drawMenu();
  drawSymbolPicker();
  drawTooltip();
  ui.inOverlayPass = false;

  if (drag.active) {
    int pid = drag.tab;
    drag.drawOverlay(ui, dock, pid >= 0 ? m_titles[(size_t)pid].c_str() : "?");
  }

  if (dock.changed) {
    saveLayout();
    dock.changed = false;
  }

  ui.end();
}

void Terminal::drawTopBar(float w) {
  const Theme& t = theme();
  Rect bar{0, 0, w, t.topBarH};
  ui.draw.rect(bar, t.bg);

  Rect logo{0, 0, 120, t.topBarH};
  ui.draw.textAligned(logo, "C E R I U M", t.accent, DrawList::Left, 14);

  // symbol button → picker popover
  Rect symBtn{130, 6, 64, t.topBarH - 12};
  if (button(ui, symBtn, kSyms[feeds.symbol])) {
    m_pickerId = ui.id("##sympicker");
    if (ui.overlayOpen(m_pickerId)) {
      ui.closeOverlay(m_pickerId);
    } else {
      m_pickerRect = {symBtn.x, t.topBarH + 4, 250, 158};
      ui.openOverlay(m_pickerId, m_pickerRect);
      // consume the opening click: when press+release land in one frame, the
      // live press would otherwise hit textField's click-outside blur below
      // and undo the autofocus
      ui.input.pressed = false;
      m_symSearch.text.clear();
      m_autoFocusPicker = true;
    }
  }
  ui.tip(ui.id("symcyc"), symBtn, "switch symbol (ETH / BTC / SOL)");

  char stats[96];
  if (m_showStats && m_renderer) {
    const Renderer::Stats& st = m_renderer->stats();
    char q[12], g[12], l[12];
    fmtCount(q, sizeof(q), st.quads);
    fmtCount(g, sizeof(g), st.glyphs);
    fmtCount(l, sizeof(l), st.lines);
    // 1s rolling frame-cost split (main.cpp) — feed drain / ui build / gpu submit
    snprintf(stats, sizeof(stats),
             "%d dc · %s q · %s g · %s l · %.1f/%.1f/%.1f ms", st.drawCalls, q, g,
             l, cerium_perf(0), cerium_perf(1), cerium_perf(2));
  } else if (themeScale() != 1.0f)
    snprintf(stats, sizeof(stats), "%.0f fps · ui %.0f%%", (double)m_fps,
             (double)(themeScale() * 100.0f));
  else
    snprintf(stats, sizeof(stats), "%.0f fps", (double)m_fps);

  // clickable: toggles fps ↔ frame stats
  Rect statsR{w - 340, 0, 220, t.topBarH};
  uint64_t sid = ui.id("##statstoggle");
  Behavior sb = behavior(ui, statsR, sid);
  if (sb.clicked) m_showStats = !m_showStats;
  ui.draw.textAligned(statsR, stats, sb.hovered ? t.text : t.textDim, DrawList::Right);
  ui.tip(sid, statsR, "frame stats (click to toggle)");

  Rect btn{w - 104, 6, 92, t.topBarH - 12};
  if (button(ui, btn, "reset layout")) {
    buildDefaultLayout();
    saveLayout();
  }
  ui.tip(ui.id("resetbtn"), btn, "restore the default layout");
}

void Terminal::handleSplitters() {
  const Theme& t = theme();
  m_splitters.clear();
  dock.collectSplitters(t.splitterSize, m_splitters);

  if (m_splitDrag) {
    DockNode* n = m_splitDrag;
    float pos = n->dir == DockDir::Horizontal
                    ? (ui.input.mouseX - n->rect.x) / n->rect.w
                    : (ui.input.mouseY - n->rect.y) / n->rect.h;
    n->ratio = pos < 0.05f ? 0.05f : (pos > 0.95f ? 0.95f : pos);
    if (!ui.input.down) {
      m_splitDrag = nullptr;
      dock.changed = true;
    }
    return;
  }

  if (ui.input.pressed) {
    for (auto& [sr, node] : m_splitters) {
      if (sr.contains(ui.input.mouseX, ui.input.mouseY)) {
        m_splitDrag = node;
        return;
      }
    }
  }
}

void Terminal::drawLeaf(DockNode* leaf) {
  const Theme& t = theme();
  Rect card = leaf->rect.inset(2.5f); // shadow room

  // raised card with soft shadow
  ui.draw.shadow(card, t.radius, 8.0f, 2.0f, hexColor(0x000000, 0.30f));
  ui.draw.rect(card, t.panel, t.radius);

  // recessed tab strip, rounded only at the card's top corners
  Rect strip{card.x, card.y, card.w, t.tabStripH};
  ui.draw.rect(strip, t.tabStrip, t.radius);
  ui.draw.rect({strip.x, strip.y + t.radius, strip.w, strip.h - t.radius}, t.tabStrip);
  drawTabStrip(leaf, strip);

  Rect content{card.x, card.y + t.tabStripH, card.w, card.h - t.tabStripH};
  if (leaf->tabs.empty()) {
    ui.draw.textAligned(content, "empty workspace", t.textDim, DrawList::Center);
  } else {
    int pid = leaf->tabs[(size_t)leaf->active];
    ui.pushId(m_titles[(size_t)pid].c_str());
    ui.draw.pushClip(content.inset(2));
    m_panels[(size_t)pid].draw(ui, content);
    ui.draw.popClip();
    ui.popId();
  }
}

void Terminal::drawTabStrip(DockNode* leaf, Rect strip) {
  const Theme& t = theme();
  int n = (int)leaf->tabs.size();
  float availW = strip.w - 8 - 26; // reserve room for the "+" affordance

  // natural widths; compress with truncation when they don't fit
  float naturalTotal = 0;
  for (int i = 0; i < n; ++i)
    naturalTotal += ui.draw.measure(m_titles[(size_t)leaf->tabs[(size_t)i]].c_str()) +
                    2 * t.pad + 20;
  float share = n > 0 ? availW / n : availW;

  ui.draw.pushClip(strip);
  float x = strip.x + 4;
  for (int i = 0; i < n; ++i) {
    int pid = leaf->tabs[(size_t)i];
    const char* title = m_titles[(size_t)pid].c_str();
    float naturalW = ui.draw.measure(title) + 2 * t.pad + 18;
    float w = naturalTotal <= availW ? naturalW : std::min(naturalW, share);
    Rect tabR{x, strip.y + 4, w, strip.h - 4};
    bool isActive = i == leaf->active;
    bool hov = ui.hovered(tabR);
    if (hov) ui.hot = ui.id(title);

    // active tab is raised to the panel tone — visually continuous with the
    // card below; no underline, no outline
    if (isActive) {
      ui.draw.rect(tabR, t.panel, 6.0f);
      ui.draw.rect({tabR.x, tabR.y + tabR.h - 6, tabR.w, 6}, t.panel);
    } else if (hov) {
      ui.draw.rect(tabR, t.bgHover, 6.0f);
    }
    // title centered in the text region (excludes the close-button zone)
    ui.draw.textFit({tabR.x, tabR.y, tabR.w - 16, tabR.h}, title,
                    isActive ? t.text : t.textDim, DrawList::Center, 4.0f);

    // close glyph (×) on the tab's right edge — font-drawn, not AA lines
    Rect closeR{tabR.x + tabR.w - 16, tabR.y, 14, tabR.h};
    bool closeHov = closeR.contains(ui.input.mouseX, ui.input.mouseY);
    ui.draw.textAligned(closeR, "\xc3\x97", closeHov ? t.red : t.textDim,
                        DrawList::Center);

    if (closeHov && ui.input.pressed) {
      dock.removeTab(leaf, pid); // collapses the leaf when emptied
    } else if (hov && ui.input.pressed) {
      leaf->active = i;
      dock.changed = true;
      drag.arm(leaf, pid, ui.input.mouseX, ui.input.mouseY);
    }
    if (hov && ui.input.rightPressed) {
      DockNode* leafPtr = leaf;
      int tabId = pid;
      openMenu(ui.input.mouseX, ui.input.mouseY,
               {{"Close tab",
                 [this, leafPtr, tabId] { dock.removeTab(leafPtr, tabId); }},
                {"Close other tabs",
                 [this, leafPtr, tabId] {
                   std::vector<int> tabs = leafPtr->tabs;
                   for (int t : tabs)
                     if (t != tabId) dock.removeTab(leafPtr, t);
                 }},
                {"Close all tabs",
                 [this, leafPtr] {
                   std::vector<int> tabs = leafPtr->tabs;
                   for (int t : tabs) dock.removeTab(leafPtr, t);
                 }}},
               m_winW, m_winH);
    }
    x += w + 4;
  }

  // "+" — add a panel that isn't currently in any leaf
  Rect plusR{x, strip.y + 4, 22, strip.h - 6};
  bool plusHov = ui.hovered(plusR);
  uint64_t plusId = ui.id("##addpanel");
  if (plusHov) {
    ui.hot = plusId;
    ui.draw.rect(plusR, t.bgHover, 4.0f);
  }
  ui.draw.textAligned(plusR, "+", plusHov ? t.text : t.textDim, DrawList::Center);
  if (plusHov && ui.input.pressed) ui.active = plusId;
  if (plusHov && ui.input.released && ui.active == plusId) {
    ui.active = 0;
    std::vector<int> presentTabs;
    dock.collectTabs(presentTabs);
    std::vector<MenuItem> items;
    for (size_t p = 0; p < m_panels.size(); ++p) {
      bool present =
          std::find(presentTabs.begin(), presentTabs.end(), (int)p) != presentTabs.end();
      if (!present) {
        DockNode* leafPtr = leaf;
        int pid = (int)p;
        items.push_back(
            {m_titles[p], [this, leafPtr, pid] { dock.insertTab(leafPtr, pid); }});
      }
    }
    if (items.empty()) items.push_back({"(all panels in use)", [] {}});
    openMenu(ui.input.mouseX, ui.input.mouseY, std::move(items), m_winW, m_winH);
  }
  ui.draw.popClip();
}

// ---------------------------------------------------------------------------
// overlay widgets (context menu, tooltip)
// ---------------------------------------------------------------------------

void Terminal::openMenu(float x, float y, std::vector<MenuItem> items, float winW,
                        float winH) {
  const Theme& t = theme();
  m_menu = std::move(items);
  float w = 0;
  for (auto& it : m_menu) w = std::max(w, ui.draw.measure(it.label.c_str()));
  w += 2 * t.pad + 20;
  float h = (float)m_menu.size() * 24.0f + 8;
  m_menuRect = {std::min(x, winW - w - 4), std::min(y, winH - h - 4), w, h};
  m_menuId = ui.id("##ctxmenu");
  ui.openOverlay(m_menuId, m_menuRect);
}

void Terminal::drawMenu() {
  if (!ui.overlayOpen(m_menuId)) {
    m_menu.clear();
    return;
  }
  const Theme& t = theme();
  Rect r = m_menuRect;
  ui.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  ui.draw.rect(r, t.panel, t.radius);

  float y = r.y + 4;
  for (size_t i = 0; i < m_menu.size(); ++i) {
    Rect item{r.x + 4, y, r.w - 8, 24};
    bool hov = ui.hovered(item);
    if (hov) {
      ui.hot = m_menuId + (uint64_t)i;
      ui.draw.rect(item, t.bgHover, 3.0f);
    }
    ui.draw.textAligned(item, m_menu[i].label.c_str(), t.text, DrawList::Left, 8);
    if (hov && ui.input.released) {
      auto action = std::move(m_menu[i].action);
      ui.closeOverlay(m_menuId);
      ui.input.released = false; // don't leak the click into panels
      action();
      return;
    }
    y += 24;
  }
}

void Terminal::drawTooltip() {
  if (!ui.pendingTip) return;
  const Theme& t = theme();
  float w = ui.draw.measure(ui.pendingTip) + 2 * t.pad;
  Rect tip{ui.tipX + 14, ui.tipY + 16, w, 24};
  if (tip.x + tip.w > m_winW) tip.x = m_winW - tip.w - 4;
  if (tip.y + tip.h > m_winH) tip.y = ui.tipY - 30;
  ui.draw.shadow(tip, 4.0f, 8.0f, 2.0f, hexColor(0x000000, 0.4f));
  ui.draw.rect(tip, t.bgRaised, 4.0f);
  ui.draw.textAligned(tip, ui.pendingTip, t.text, DrawList::Center);
}

static bool containsCI(const char* hay, const std::string& needle) {
  if (needle.empty()) return true;
  size_t nl = needle.size();
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == nl) return true;
  }
  return false;
}

void Terminal::drawSymbolPicker() {
  if (!ui.overlayOpen(m_pickerId)) {
    if (m_symSearch.focused) { // picker closed while the field was focused
      m_symSearch.focused = false;
      shell_ime_blur();
    }
    return;
  }
  const Theme& t = theme();
  Rect r = m_pickerRect;

  ui.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  ui.draw.rect(r, t.panel, t.radius);

  Rect searchR{r.x + 8, r.y + 8, r.w - 16, 26};
  if (m_autoFocusPicker) {
    shell_ime_set("");
    shell_ime_focus(searchR.x, searchR.y, searchR.w, searchR.h);
    m_symSearch.focused = true;
    m_autoFocusPicker = false;
  }
  textField(ui, searchR, m_symSearch, "##symsearch", "search symbol…");

  // venue support per symbol — mirrors feeds/registry.ts
  static const int kVenueCount[3] = {27, 24, 24};
  int matches[3];
  int nm = 0;
  for (int i = 0; i < 3; ++i)
    if (containsCI(kSyms[i], m_symSearch.text)) matches[nm++] = i;

  if (m_symSearch.submitted) {
    m_symSearch.submitted = false;
    if (nm > 0) {
      feeds.setSymbol(matches[0]);
      ui.closeOverlay(m_pickerId);
      return;
    }
  }

  float y = searchR.y + searchR.h + 6;
  for (int j = 0; j < nm; ++j) {
    int sym = matches[j];
    Rect row{r.x + 6, y, r.w - 12, 24};
    bool hov = ui.hovered(row);
    if (hov) {
      ui.hot = m_pickerId + (uint64_t)sym;
      ui.draw.rect(row, t.bgHover, 3.0f);
    }
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d venues", kVenueCount[sym]);
    ui.draw.textAligned(row, kSyms[sym], sym == feeds.symbol ? t.accent : t.text,
                        DrawList::Left, 8);
    ui.draw.textAligned(row, cnt, t.textDim, DrawList::Right, 8);
    if (hov && ui.input.released) {
      feeds.setSymbol(sym);
      ui.closeOverlay(m_pickerId);
      ui.input.released = false;
      return;
    }
    y += 24;
  }
}

// ---------------------------------------------------------------------------
// chart panel hookup (panel draw lives in chart/chart_panel.cpp)
// ---------------------------------------------------------------------------

void Terminal::drawChart(Ui& u, Rect r) {
  m_chart.draw(u, r, feeds,
               [this](float x, float y, std::vector<ChartMenuItem> items) {
                 std::vector<MenuItem> menu;
                 menu.reserve(items.size());
                 for (auto& it : items)
                   menu.push_back({std::move(it.label), std::move(it.action)});
                 openMenu(x, y, std::move(menu), m_winW, m_winH);
               });
}
