#include "terminal.h"

#include "../dock/dock_layout.h"
#include "../platform/shell.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "symbols.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// 1s rolling frame-cost window maintained by main.cpp (index → metric)
extern "C" double cerium_perf(int i);

static const char* kLayoutKey = "cerium.layout.v1";
static const char* kTabCloseKey = "cerium.tabs.close.visible.v1";
static const char* kTabStripsKey = "cerium.tabs.visible.v1";

static constexpr PanelKind kPanelKinds[] = {
    PanelKind::Chart, PanelKind::Orderbook, PanelKind::Dom, PanelKind::Tape,
    PanelKind::Liquidations, PanelKind::Watchlist, PanelKind::Feeds};

static const char* panelKindTitle(PanelKind kind) {
  switch (kind) {
    case PanelKind::Chart: return "Chart";
    case PanelKind::Orderbook: return "Orderbook";
    case PanelKind::Dom: return "DOM";
    case PanelKind::Tape: return "Tape";
    case PanelKind::Liquidations: return "Liquidations";
    case PanelKind::Watchlist: return "Watchlist";
    case PanelKind::Feeds: return "Feeds";
  }
  return "Panel";
}

static bool titleKind(const std::string& title, PanelKind& kind, int& ordinal) {
  for (PanelKind candidate : kPanelKinds) {
    const std::string base = panelKindTitle(candidate);
    if (title == base) {
      kind = candidate;
      ordinal = 1;
      return true;
    }
    if (title.size() <= base.size() + 1 || title.compare(0, base.size(), base) != 0 ||
        title[base.size()] != ' ')
      continue;
    char* end = nullptr;
    long value = std::strtol(title.c_str() + base.size() + 1, &end, 10);
    if (end && *end == '\0' && value >= 2 && value <= 1000000) {
      kind = candidate;
      ordinal = (int)value;
      return true;
    }
  }
  return false;
}

static std::string panelSettingsKey(PanelKind kind, int ordinal) {
  const char* base = kind == PanelKind::Chart       ? "cerium.chart.settings.v1"
                     : kind == PanelKind::Orderbook ? "cerium.orderbook.settings.v1"
                     : kind == PanelKind::Dom       ? "cerium.dom.settings.v1"
                     : kind == PanelKind::Tape      ? "cerium.tape.settings.v1"
                     : kind == PanelKind::Liquidations ? "cerium.liquidations.settings.v1"
                                                    : "";
  if (!*base || ordinal == 1) return base;
  return std::string(base) + "." + std::to_string(ordinal);
}

int Terminal::addPanel(PanelKind kind, const std::string& requestedTitle) {
  std::string title = requestedTitle;
  if (title.empty()) {
    title = panelKindTitle(kind);
    if (panelId(title.c_str()) >= 0) {
      for (int n = 2;; ++n) {
        title = std::string(panelKindTitle(kind)) + " " + std::to_string(n);
        if (panelId(title.c_str()) < 0) break;
      }
    }
  }

  PanelKind parsedKind = kind;
  int ordinal = 1;
  if (!titleKind(title, parsedKind, ordinal) || parsedKind != kind) return -1;
  int id = (int)m_panels.size();
  std::function<void(Ui&, Rect)> draw;
  if (kind == PanelKind::Chart) {
    auto state = std::make_unique<ChartPanel>();
    state->setStorageKey(panelSettingsKey(kind, ordinal));
    ChartPanel* ptr = state.get();
    m_charts.push_back(std::move(state));
    draw = [this, ptr](Ui& u, Rect r) { ptr->draw(u, r, feeds); };
  } else if (kind == PanelKind::Orderbook) {
    auto state = std::make_unique<OrderbookPanel>();
    state->settingsKey = panelSettingsKey(kind, ordinal);
    OrderbookPanel* ptr = state.get();
    m_orderbooks.push_back(std::move(state));
    draw = [this, ptr](Ui& u, Rect r) { drawOrderbook(u, r, *ptr, feeds); };
  } else if (kind == PanelKind::Dom) {
    auto state = std::make_unique<DomPanel>();
    state->settingsKey = panelSettingsKey(kind, ordinal);
    DomPanel* ptr = state.get();
    m_doms.push_back(std::move(state));
    draw = [this, ptr](Ui& u, Rect r) { drawDom(u, r, *ptr, feeds); };
  } else if (kind == PanelKind::Tape) {
    auto state = std::make_unique<TapePanel>();
    state->settingsKey = panelSettingsKey(kind, ordinal);
    TapePanel* ptr = state.get();
    m_tapes.push_back(std::move(state));
    draw = [this, ptr](Ui& u, Rect r) { drawTape(u, r, *ptr, feeds); };
  } else if (kind == PanelKind::Liquidations) {
    auto state = std::make_unique<LiquidationsPanel>();
    state->settingsKey = panelSettingsKey(kind, ordinal);
    LiquidationsPanel* ptr = state.get();
    m_liquidations.push_back(std::move(state));
    draw = [this, ptr](Ui& u, Rect r) { drawLiquidations(u, r, *ptr, feeds); };
  } else if (kind == PanelKind::Feeds) {
    auto state = std::make_unique<FeedsPanel>();
    FeedsPanel* ptr = state.get();
    m_feedPanels.push_back(std::move(state));
    draw = [this, ptr](Ui& u, Rect r) { drawFeeds(u, r, *ptr, feeds); };
  } else {
    draw = [this](Ui& u, Rect r) { drawWatchlist(u, r, feeds); };
  }

  m_panels.push_back({id, kind, title, std::move(draw)});
  m_titles.push_back(title);
  return id;
}

int Terminal::addWidget(PanelKind kind, DockNode* host) {
  if (!host) return -1;
  static thread_local std::vector<int> present;
  present.clear();
  dock.collectTabs(present);
  for (const PanelDef& panel : m_panels) {
    bool inDock = std::find(present.begin(), present.end(), panel.id) != present.end();
    if (panel.kind == kind && !inDock) {
      dock.insertTab(host, panel.id);
      return panel.id;
    }
  }
  int id = addPanel(kind);
  if (id >= 0) dock.insertTab(host, id);
  return id;
}

void Terminal::ensurePanelsForLayout(const char* json) {
  std::vector<std::string> titles;
  dockCollectTabTitles(json, titles);
  for (const std::string& title : titles) {
    if (panelId(title.c_str()) >= 0) continue;
    PanelKind kind;
    int ordinal = 0;
    if (titleKind(title, kind, ordinal)) addPanel(kind, title);
  }
}

int Terminal::debugLadderLevels() const {
  return m_orderbooks.empty() ? 0 : (int)m_orderbooks.front()->ladder.size();
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

  for (PanelKind kind : kPanelKinds) addPanel(kind);

  buildDefaultLayout();
  feeds.init();
  loadLayoutIndex();

  if (char* sc = shell_storage_get("cerium.uiscale")) {
    float s = (float)atof(sc);
    if (s >= 0.5f && s <= 2.5f) themeApplyScale(s);
    free(sc);
  }
  if (char* visible = shell_storage_get(kTabCloseKey)) {
    m_showTabClose = visible[0] != '0';
    free(visible);
  }
  if (char* visible = shell_storage_get(kTabStripsKey)) {
    m_showTabStrips = visible[0] != '0';
    free(visible);
  }
}

void Terminal::buildDefaultLayout() {
  DockNode* chart = dock.makeLeaf({panelId("Chart")});
  DockNode* ob = dock.makeLeaf({panelId("Orderbook"), panelId("DOM")});
  DockNode* tape = dock.makeLeaf({panelId("Tape"), panelId("Liquidations"),
                                  panelId("Watchlist"), panelId("Feeds")});
  DockNode* right = dock.makeSplit(DockDir::Vertical, ob, tape, 0.5f);
  dock.setRoot(dock.makeSplit(DockDir::Horizontal, chart, right, 0.66f));
}

void Terminal::restoreLayout() {
  char* json = shell_storage_get(kLayoutKey);
  if (json) {
    ensurePanelsForLayout(json);
    if (dockDeserialize(dock, json, m_titles)) {
      free(json);
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

void Terminal::frame(const Input& input, float frameDt, float uiDt,
                     float cssW, float cssH) {
  if (!m_restored) {
    restoreLayout();
    m_restored = true;
  }
  if (frameDt > 0) m_fps = m_fps * 0.95f + (1.0f / frameDt) * 0.05f;

  ui.begin(input, uiDt);
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
  } else if (std::any_of(m_charts.begin(), m_charts.end(),
                         [](const auto& chart) { return chart->paneResizing(); })) {
    cursor = "row-resize";
  } else if (std::any_of(m_charts.begin(), m_charts.end(),
                         [](const auto& chart) { return chart->panning(); })) {
    cursor = "grabbing";
  } else if (std::any_of(m_charts.begin(), m_charts.end(),
                         [](const auto& chart) { return chart->drawing(); })) {
    cursor = "crosshair";
    for (const auto& chart : m_charts) {
      if (!chart->drawing()) continue;
      if (const char* dc = chart->drawingCursor()) {
        cursor = dc;
        break;
      }
    }
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
  drawSymbolPicker();
  drawWidgetsPicker();
  drawLayoutsMenu();
  for (auto& chart : m_charts) {
    chart->drawIndicatorPicker(ui);
    chart->drawIndicatorSettings(ui);
    chart->drawTfPicker(ui, feeds);
    chart->drawFlowPicker(ui, feeds);
    chart->drawToolPicker(ui);
  }
  for (auto& dom : m_doms) dom->drawVenuePicker(ui, feeds);
  for (auto& ob : m_orderbooks) ob->drawFlowPicker(ui, feeds);
  for (auto& dom : m_doms) dom->drawFlowPicker(ui, feeds);
  drawMenu(); // context menus stack above the other popups
  drawTooltip();
  ui.inOverlayPass = false;

  if (drag.active) {
    int pid = drag.tab;
    drag.drawOverlay(ui, dock, pid >= 0 ? m_titles[(size_t)pid].c_str() : "?");
  }

  // Debounced persistence: tab activation marks the dock dirty on every
  // click; serialize+write only after ~1 s without changes so switching
  // tabs doesn't write localStorage per frame-click.
  if (dock.changed && ui.time - m_layoutDirtyAt >= 1.0) {
    saveLayout();
    dock.changed = false;
  }

  ui.end();
}

void Terminal::drawTopBar(float w) {
  const Theme& t = theme();
  Rect bar{0, 0, w, t.topBarH};
  ui.draw.rect(bar, t.bg);

  auto separator = [&](float x) {
    ui.draw.rect({x, 0, 1, t.topBarH - 1}, t.border);
  };

  // Compact terminal identity and market selector. Functional groups use
  // full-height separators so the bar reads as one command surface rather
  // than a set of unrelated floating labels.
  ui.draw.setFont(FontMonoSemibold);
  ui.draw.textAligned({14, 0, 66, t.topBarH}, "CERIUM", t.text, DrawList::Left);
  ui.draw.setFont(FontMono);
  separator(82);

  // symbol button → picker popover
  Rect symBtn{83, 1, 130, t.topBarH - 2};
  uint64_t symId = ui.id("##symbol");
  Behavior sym = behavior(ui, symBtn, symId);
  if (sym.held || sym.hovered)
    ui.draw.rect(symBtn, sym.held ? t.accentSoft : t.bgHover);
  ui.draw.setFont(FontMonoSemibold);
  ui.draw.textAligned({symBtn.x + 12, symBtn.y, 34, symBtn.h}, symbols::kNames[feeds.symbol],
                      t.accent, DrawList::Left);
  ui.draw.setFont(FontMono);
  Color quietText = withAlpha(t.text, 0.60f);
  ui.draw.textAligned({symBtn.x + 47, symBtn.y, 55, symBtn.h}, "/ USDT", quietText,
                      DrawList::Left);
  Color caret = sym.hovered ? t.text : quietText;
  float cx = symBtn.x + symBtn.w - 14, cy = symBtn.y + symBtn.h * 0.5f;
  ui.draw.line(cx - 3, cy - 1, cx, cy + 2, caret, 1.0f);
  ui.draw.line(cx, cy + 2, cx + 3, cy - 1, caret, 1.0f);
  if (sym.clicked) {
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
  ui.tip(symId, symBtn, "switch market (ETH / BTC / SOL)");
  separator(symBtn.x + symBtn.w);

  char stats[72];
  if (m_showStats && m_renderer) {
    const Renderer::Stats& st = m_renderer->stats();
    double frameMs = cerium_perf(0) + cerium_perf(1) + cerium_perf(2);
    snprintf(stats, sizeof(stats), "%d draws  /  %.1f ms", st.drawCalls, frameMs);
  } else {
    int live = feeds.liveCount();
    if (themeScale() != 1.0f)
      snprintf(stats, sizeof(stats), "%d feeds  /  %.0f fps  /  %.0f%%", live,
               (double)m_fps, (double)(themeScale() * 100.0f));
    else
      snprintf(stats, sizeof(stats), "%d feeds  /  %.0f fps", live, (double)m_fps);
  }

  const float resetW = 76.0f;
  const float layoutsW = 92.0f;
  const float tabsW = 72.0f;
  const float panelW = 88.0f;
  const float statsW = 176.0f;
  const float activeW = 118.0f;
  Rect resetBtn{w - resetW, 1, resetW, t.topBarH - 2};
  Rect layoutsBtn{resetBtn.x - layoutsW, 1, layoutsW, t.topBarH - 2};
  Rect tabsBtn{layoutsBtn.x - tabsW, 1, tabsW, t.topBarH - 2};
  Rect addBtn{tabsBtn.x - panelW, 1, panelW, t.topBarH - 2};

  // Runtime telemetry is deliberately textual—no decorative health light.
  // Click the segment to switch between feed/fps and frame-cost readouts.
  if (w > 720) {
    Rect statsR{addBtn.x - statsW, 1, statsW, t.topBarH - 2};
    uint64_t sid = ui.id("##statstoggle");
    Behavior sb = behavior(ui, statsR, sid);
    if (sb.clicked) m_showStats = !m_showStats;
    ui.draw.rect(statsR, sb.hovered ? t.bgHover : t.panelAlt);
    ui.draw.textAligned(statsR, stats, sb.hovered ? t.text : t.textDim,
                        DrawList::Center);
    separator(statsR.x);
    ui.tip(sid, statsR, "runtime stats (click to toggle)");

    // Active snapshot is a quiet workspace breadcrumb in its own segment.
    if (!m_activeLayout.empty() && w > 980) {
      Rect activeR{statsR.x - activeW, 1, activeW, t.topBarH - 2};
      ui.draw.textFit(activeR, m_activeLayout.c_str(), t.accent,
                      DrawList::Center, 10);
      separator(activeR.x);
    }
  }

  // Persistent low-contrast cells keep workspace actions discoverable. The
  // active overlay uses the accent, while hover remains a tone shift only.
  auto topAction = [&](Rect r, const char* id, const char* label, bool active) {
    Behavior b = behavior(ui, r, ui.id(id));
    Color bg = b.held || active ? t.accentSoft : b.hovered ? t.bgHover : t.panelAlt;
    ui.draw.rect(r, bg);
    ui.draw.setFont(FontMonoSemibold);
    ui.draw.textAligned(r, label, active ? t.accent : b.hovered ? t.text : quietText,
                        DrawList::Center);
    ui.draw.setFont(FontMono);
    separator(r.x);
    return b.clicked;
  };

  // Panel → popup creates/reopens independent widget instances and carries the
  // global tab-close visibility preference.
  if (!m_widgetsId) m_widgetsId = ui.id("##widgets");
  if (topAction(addBtn, "##top-add", "Panel", ui.overlayOpen(m_widgetsId))) {
    if (ui.overlayOpen(m_widgetsId)) {
      ui.closeOverlay(m_widgetsId);
    } else {
      const float pw = 220.0f;
      const float ph = 32.0f + (float)(sizeof(kPanelKinds) / sizeof(kPanelKinds[0])) * 24.0f + 16.0f;
      m_widgetsRect = {addBtn.x + addBtn.w - pw, t.topBarH + 4, pw, ph};
      ui.openOverlay(m_widgetsId, m_widgetsRect);
      ui.input.pressed = false;
    }
  }

  // Global workspace chrome toggle. The dock tree and active tabs remain
  // untouched; hiding strips simply returns their height to panel content.
  if (topAction(tabsBtn, "##top-tabs", "Tabs", !m_showTabStrips)) {
    m_showTabStrips = !m_showTabStrips;
    shell_storage_set(kTabStripsKey, m_showTabStrips ? "1" : "0");
    drag.cancel();
  }
  ui.tip(ui.id("##top-tabs"), tabsBtn,
         m_showTabStrips ? "hide widget tab bars" : "show widget tab bars");

  // Layouts → snapshot manager popup.
  if (!m_layoutsId) m_layoutsId = ui.id("##layouts");
  if (topAction(layoutsBtn, "##top-layouts", "Layouts",
                ui.overlayOpen(m_layoutsId))) {
    if (ui.overlayOpen(m_layoutsId)) {
      ui.closeOverlay(m_layoutsId);
    } else {
      m_layoutsRect = {layoutsBtn.x + layoutsBtn.w - 300, t.topBarH + 4, 300, 120};
      ui.openOverlay(m_layoutsId, m_layoutsRect); // height syncs per frame
      ui.input.pressed = false;
      m_layoutError.clear();
      m_renaming = -1;
    }
  }

  if (topAction(resetBtn, "##top-reset", "Reset", false)) {
    buildDefaultLayout();
    saveLayout();
  }
  ui.draw.rect({0, t.topBarH - 1, w, 1}, t.border);
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
      m_layoutDirtyAt = ui.time;
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
  Rect card = leaf->rect;

  // Flat, contiguous work surface. Dock gaps are the only panel separators.
  ui.draw.rect(card, t.panel);

  Rect content = card;
  if (m_showTabStrips) {
    Rect strip{card.x, card.y, card.w, t.tabStripH};
    ui.draw.rect(strip, t.tabStrip);
    ui.draw.rect({strip.x, strip.y + strip.h - 1, strip.w, 1}, t.border);
    drawTabStrip(leaf, strip);
    content = {card.x, card.y + t.tabStripH, card.w, card.h - t.tabStripH};
  }
  if (leaf->tabs.empty()) {
    ui.draw.textAligned(content, "empty workspace", t.textDim, DrawList::Center);
  } else {
    int pid = leaf->tabs[(size_t)leaf->active];
    ui.pushId(m_titles[(size_t)pid].c_str());
    ui.draw.pushClip(content);
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
  const bool allowClose = m_showTabClose &&
                          (n <= 0 || availW / std::max(1, n) >= 48.0f);
  const float closeSpace = allowClose ? 24.0f : 10.0f;
  for (int i = 0; i < n; ++i)
    naturalTotal += ui.draw.measure(m_titles[(size_t)leaf->tabs[(size_t)i]].c_str()) +
                    2 * t.pad + closeSpace;
  float share = n > 0 ? availW / n : availW;

  ui.draw.pushClip(strip);
  float x = strip.x + 4;
  for (int i = 0; i < n; ++i) {
    int pid = leaf->tabs[(size_t)i];
    const char* title = m_titles[(size_t)pid].c_str();
    float naturalW = ui.draw.measure(title) + 2 * t.pad + closeSpace;
    float w = naturalTotal <= availW ? naturalW : std::min(naturalW, share);
    Rect tabR{x, strip.y, w, strip.h - 1};
    bool isActive = i == leaf->active;
    bool hov = ui.hovered(tabR);
    if (hov) ui.hot = ui.id(title);

    // Active tab joins the work surface; inactive tabs stay in navigation tone.
    if (isActive) {
      ui.draw.rect(tabR, t.panel);
    } else if (hov) {
      ui.draw.rect(tabR, t.bgHover);
    }
    // Left-aligned labels scan like workspace tabs, not segmented game buttons.
    ui.draw.textFit({tabR.x, tabR.y,
                     tabR.w - (allowClose ? 16.0f : 4.0f), tabR.h}, title,
                    isActive ? t.text : t.textDim, DrawList::Left, 8.0f);

    // close glyph (×) on the tab's right edge — font-drawn, not AA lines
    Rect closeR{tabR.x + tabR.w - 16, tabR.y, 14, tabR.h};
    bool closeHov = allowClose &&
                    closeR.contains(ui.input.mouseX, ui.input.mouseY);
    if (allowClose && (isActive || hov))
      ui.draw.textAligned(closeR, "\xc3\x97", closeHov ? t.red : t.textDim,
                          DrawList::Center);

    if (closeHov && ui.input.pressed) {
      dock.removeTab(leaf, pid); // collapses the leaf when emptied
      // tabs shifted left under this loop — one close per press
      m_layoutDirtyAt = ui.time;
      break;
    } else if (hov && ui.input.pressed) {
      leaf->active = i;
      dock.changed = true;
      m_layoutDirtyAt = ui.time;
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
    x += w + 2;
  }

  // "+" — add another independent widget instance to this leaf. A closed
  // instance of that type is reused first so its local state is not lost.
  Rect plusR{x, strip.y, 22, strip.h - 1};
  bool plusHov = ui.hovered(plusR);
  uint64_t plusId = ui.id("##addpanel");
  if (plusHov) {
    ui.hot = plusId;
    ui.draw.rect(plusR, t.bgHover);
  }
  ui.draw.textAligned(plusR, "+", plusHov ? t.text : t.textDim, DrawList::Center);
  if (plusHov && ui.input.pressed) ui.active = plusId;
  if (plusHov && ui.input.released && ui.active == plusId) {
    ui.active = 0;
    std::vector<MenuItem> items;
    for (PanelKind kind : kPanelKinds) {
      DockNode* leafPtr = leaf;
      items.push_back({panelKindTitle(kind),
                       [this, leafPtr, kind] { addWidget(kind, leafPtr); }});
    }
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
  ui.draw.rectOutline(r, t.border, 1.0f, t.radius);

  float y = r.y + 4;
  for (size_t i = 0; i < m_menu.size(); ++i) {
    Rect item{r.x + 4, y, r.w - 8, 24};
    bool hov = ui.hovered(item);
    if (hov) {
      ui.hot = m_menuId + (uint64_t)i;
      ui.draw.rect(item, t.bgHover, 3.0f);
    }
    ui.draw.textAligned(item, m_menu[i].label.c_str(), t.text, DrawList::Left, 8);
    // Press-inside guard: menu items commit only when the press started on
    // the item itself (a drag released over the menu must not fire it).
    if (behavior(ui, item, m_menuId + (uint64_t)i + 1).clicked) {
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
  ui.draw.rectOutline(r, t.border, 1.0f, t.radius);

  Rect searchR{r.x + 8, r.y + 8, r.w - 16, 26};
  if (m_autoFocusPicker) {
    shell_ime_set("");
    shell_ime_focus(searchR.x, searchR.y, searchR.w, searchR.h);
    m_symSearch.focused = true;
    m_autoFocusPicker = false;
  }
  textField(ui, searchR, m_symSearch, "##symsearch", "search symbol…");

  // venue support per symbol — mirrors feeds/registry.ts
    int matches[3];
  int nm = 0;
  for (int i = 0; i < 3; ++i)
    if (containsCI(symbols::kNames[i], m_symSearch.text)) matches[nm++] = i;

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
    snprintf(cnt, sizeof(cnt), "%d venues", symbols::kVenueCounts[sym]);
    ui.draw.textAligned(row, symbols::kNames[sym], sym == feeds.symbol ? t.accent : t.text,
                        DrawList::Left, 8);
    ui.draw.textAligned(row, cnt, t.textDim, DrawList::Right, 8);
    if (behavior(ui, row, m_pickerId ^ (0x9E3779B97F4A7C15ull * (uint64_t)(sym + 1))).clicked) {
      feeds.setSymbol(sym);
      ui.closeOverlay(m_pickerId);
      ui.input.released = false;
      return;
    }
    y += 24;
  }
}

// ---------------------------------------------------------------------------
// add-widget popup (overlay)
// ---------------------------------------------------------------------------

// Fixed widget catalog. Clicking a type always adds another instance (or
// reopens the first closed instance of that type), so the registry can grow
// without turning this menu into an unbounded list of numbered copies.
void Terminal::drawWidgetsPicker() {
  if (!ui.overlayOpen(m_widgetsId)) return;
  const Theme& t = theme();
  Rect r = m_widgetsRect;
  ui.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  ui.draw.rect(r, t.panel, t.radius);
  ui.draw.rectOutline(r, t.border, 1.0f, t.radius);

  static thread_local std::vector<int> present;
  present.clear();
  dock.collectTabs(present);

  float y = r.y + 4;
  Rect closeRow{r.x + 4, y, r.w - 8, 24};
  bool closeHov = ui.hovered(closeRow);
  if (closeHov) {
    ui.hot = m_widgetsId + 100;
    ui.draw.rect(closeRow, t.bgHover, 3.0f);
  }
  ui.draw.textAligned(closeRow, "TAB CLOSE BUTTONS", t.text, DrawList::Left, 8);
  ui.draw.textAligned(closeRow, m_showTabClose ? "VISIBLE" : "HIDDEN",
                      m_showTabClose ? t.accent : t.textDim, DrawList::Right, 8);
  if (closeHov && ui.input.released) {
    m_showTabClose = !m_showTabClose;
    shell_storage_set(kTabCloseKey, m_showTabClose ? "1" : "0");
    ui.input.released = false;
  }
  y += 28;
  ui.draw.rect({r.x + 8, y - 2, r.w - 16, 1}, t.border);

  for (size_t i = 0; i < sizeof(kPanelKinds) / sizeof(kPanelKinds[0]); ++i) {
    PanelKind kind = kPanelKinds[i];
    int open = 0;
    for (int id : present)
      if (id >= 0 && id < (int)m_panels.size() && m_panels[(size_t)id].kind == kind)
        ++open;
    Rect row{r.x + 4, y, r.w - 8, 24};
    bool hov = ui.hovered(row);
    if (hov) {
      ui.hot = m_widgetsId + (uint64_t)i + 1;
      ui.draw.rect(row, t.bgHover, 3.0f);
    }
    ui.draw.textAligned(row, panelKindTitle(kind), t.text, DrawList::Left, 8);
    char count[24];
    snprintf(count, sizeof(count), "%d OPEN", open);
    ui.draw.textAligned(row, count, open > 0 ? t.accent : t.textDim,
                        DrawList::Right, 8);
    if (hov && ui.input.released) {
      DockNode* host = dock.firstLeaf();
      addWidget(kind, host);
      ui.closeOverlay(m_widgetsId);
      ui.input.released = false;
      return;
    }
    y += 24;
  }
}
