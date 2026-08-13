#include "terminal.h"

#include "../dock/dock_layout.h"
#include "../platform/shell.h"
#include "../ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static const char* kLayoutKey = "cerium.layout.v1";

// deterministic pseudo-random per (i, tick) — drives the fake market data
static float frand(uint32_t i, uint32_t tick) {
  uint32_t h = i * 2654435761u ^ tick * 2246822519u;
  h ^= h >> 13;
  h *= 3266489917u;
  h ^= h >> 16;
  return (float)(h & 0xffff) / 65535.0f;
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

void Terminal::init(GlyphAtlas* atlas) {
  ui.init(atlas);

  addPanel("Chart", [this](Ui& u, Rect r) { drawChart(u, r); });
  addPanel("Orderbook", [this](Ui& u, Rect r) { drawOrderbook(u, r); });
  addPanel("Tape", [this](Ui& u, Rect r) { drawTape(u, r); });
  addPanel("Watchlist", [this](Ui& u, Rect r) { drawWatchlist(u, r); });

  buildDefaultLayout();
  feeds.init();
}

void Terminal::buildDefaultLayout() {
  DockNode* chart = dock.makeLeaf({panelId("Chart")});
  DockNode* ob = dock.makeLeaf({panelId("Orderbook")});
  DockNode* tape = dock.makeLeaf({panelId("Tape"), panelId("Watchlist")});
  DockNode* right = dock.makeSplit(DockDir::Vertical, ob, tape, 0.5f);
  dock.setRoot(dock.makeSplit(DockDir::Horizontal, chart, right, 0.66f));
}

void Terminal::restoreLayout() {
  char* json = shell_storage_get(kLayoutKey);
  if (json) {
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

void Terminal::frame(const Input& input, float dt, float cssW, float cssH) {
  if (!m_restored) {
    restoreLayout();
    m_restored = true;
  }
  if (dt > 0) m_fps = m_fps * 0.95f + (1.0f / dt) * 0.05f;

  ui.begin(input, dt);
  const Theme& t = theme();

  feeds.frame(); // drain the wire ring into books/tape

  drawTopBar(cssW);

  Rect area{0, t.topBarH, cssW, cssH - t.topBarH};
  dock.computeRects(area, t.splitterSize);

  if (drag.armed || drag.active) {
    drag.update(ui, dock);
  } else {
    handleSplitters();
  }

  // panels
  std::vector<DockNode*> stack{dock.root};
  while (!stack.empty()) {
    DockNode* n = stack.back();
    stack.pop_back();
    if (!n) continue;
    if (n->isLeaf()) drawLeaf(n);
    else {
      stack.push_back(n->a);
      stack.push_back(n->b);
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
        ui.draw.rect(sr, t.splitter);
        cursor = node->dir == DockDir::Horizontal ? "col-resize" : "row-resize";
      }
    }
    if (cursor[0] == 'd' && ui.hot) cursor = "pointer";
  }
  shell_set_cursor(cursor);

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
  ui.draw.rect(bar, t.panelAlt);
  ui.draw.rect({0, t.topBarH - 1, w, 1}, t.border);

  Rect logo{0, 0, 120, t.topBarH};
  ui.draw.textAligned(logo, "C E R I U M", t.accent, DrawList::Left, 14);

  char stats[32];
  snprintf(stats, sizeof(stats), "%.0f fps", (double)m_fps);
  Rect statsR{w - 220, 0, 100, t.topBarH};
  ui.draw.textAligned(statsR, stats, t.textDim, DrawList::Right);

  Rect btn{w - 104, 6, 92, t.topBarH - 12};
  if (button(ui, btn, "reset layout")) {
    buildDefaultLayout();
    saveLayout();
  }
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
  Rect r = leaf->rect;

  ui.draw.rect(r, t.panel);

  Rect strip{r.x, r.y, r.w, t.tabStripH};
  ui.draw.rect(strip, t.tabStrip);
  ui.draw.rect({strip.x, strip.y + strip.h - 1, strip.w, 1}, t.border);
  drawTabStrip(leaf, strip);

  Rect content{r.x, r.y + t.tabStripH, r.w, r.h - t.tabStripH};
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

  ui.draw.rectOutline(r, t.border, 1.0f);
}

void Terminal::drawTabStrip(DockNode* leaf, Rect strip) {
  const Theme& t = theme();
  float x = strip.x + 4;
  for (int i = 0; i < (int)leaf->tabs.size(); ++i) {
    int pid = leaf->tabs[(size_t)i];
    const char* title = m_titles[(size_t)pid].c_str();
    float w = ui.draw.measure(title) + 2 * t.pad + 8;
    Rect tabR{x, strip.y + 4, w, strip.h - 4};
    bool isActive = i == leaf->active;
    bool hov = ui.hovered(tabR);
    if (hov) ui.hot = ui.id(title);

    // active tab connects visually into the panel below
    ui.draw.rect(tabR, isActive ? t.panel : (hov ? t.bgHover : t.tabStrip), 4.0f);
    if (isActive) {
      ui.draw.rect({tabR.x + 1, tabR.y + tabR.h - 1, tabR.w - 1, 2}, t.panel);
      ui.draw.rect({tabR.x + 6, tabR.y + tabR.h - 2, tabR.w - 12, 2}, t.accent, 1.0f);
    }
    ui.draw.textAligned(tabR, title, isActive ? t.text : t.textDim, DrawList::Center);

    if (hov && ui.input.pressed) {
      leaf->active = i;
      dock.changed = true;
      drag.arm(leaf, pid, ui.input.mouseX, ui.input.mouseY);
    }
    x += w + 4;
  }
}

// ---------------------------------------------------------------------------
// demo panels (stand-ins for the real widgets)
// ---------------------------------------------------------------------------

void Terminal::drawChart(Ui& u, Rect r) {
  const Theme& t = theme();
  uint32_t tick = (uint32_t)(u.time * 2.0f);

  for (int i = 1; i < 6; ++i)
    u.draw.rect({r.x, r.y + r.h * i / 6.0f, r.w, 1}, t.border);
  for (int i = 1; i < 8; ++i)
    u.draw.rect({r.x + r.w * i / 8.0f, r.y, 1, r.h}, t.border);

  const int n = 64;
  float bw = r.w / n;
  float lastY = 0, lastPrice = 0;
  for (int i = 0; i < n; ++i) {
    float o = frand((uint32_t)i, tick);
    float c = frand((uint32_t)i, tick + 1);
    float hi = std::min(o, c) - 0.06f * frand((uint32_t)i, tick + 2);
    float lo = std::max(o, c) + 0.06f * frand((uint32_t)i, tick + 3);
    // map value 0..1 into 12%..92% of panel height
    auto fy = [](float v) { return 0.12f + 0.80f * v; };
    float yO = r.y + fy(o) * r.h, yC = r.y + fy(c) * r.h;
    float yH = r.y + fy(hi) * r.h, yL = r.y + fy(lo) * r.h;
    bool up = yC < yO;
    Color col = up ? t.green : t.red;

    float cx = r.x + i * bw + bw * 0.5f;
    u.draw.rect({cx - 0.5f, yH, 1.0f, std::max(yL - yH, 1.0f)}, withAlpha(col, 0.9f));
    float bodyY = std::min(yO, yC);
    u.draw.rect({r.x + i * bw + bw * 0.2f, bodyY, bw * 0.6f,
                 std::max(std::fabs(yO - yC), 1.0f)},
                withAlpha(col, 0.85f));

    if (i == n - 1) {
      lastY = yC;
      lastPrice = 4300.0f + (0.5f - fy(c)) * 400.0f;
    }
  }

  // last-price line + tag
  u.draw.rect({r.x, lastY, r.w, 1}, withAlpha(t.accent, 0.5f));
  char lp[32];
  snprintf(lp, sizeof(lp), "%.2f", (double)lastPrice);
  float lw = u.draw.measure(lp) + 14;
  Rect tag{r.x + r.w - lw - 8, lastY - 9, lw, 18};
  u.draw.rect(tag, t.bgRaised, 3.0f);
  u.draw.rectOutline(tag, t.accent, 1.0f, 3.0f);
  u.draw.textAligned(tag, lp, t.accent, DrawList::Center);

  // header
  Rect hdr{r.x, r.y, r.w, 26};
  u.draw.textAligned(hdr, "ETHUSDT", t.text, DrawList::Left, 12);
  float symW = u.draw.measure("ETHUSDT");
  u.draw.textAligned({r.x + 12 + symW + 14, r.y, 220, 26}, "Perpetual · Binance",
                     t.textDim, DrawList::Left);
}

static const char* statusText(uint8_t s) {
  switch (s) {
    case wire::Connecting: return "connecting";
    case wire::Syncing: return "syncing";
    case wire::Live: return "live";
    case wire::Reconnecting: return "reconnecting";
    case wire::Error: return "error";
    default: return "offline";
  }
}

static Color statusColor(uint8_t s, const Theme& t) {
  switch (s) {
    case wire::Live: return t.green;
    case wire::Connecting:
    case wire::Syncing: return t.accent;
    case wire::Reconnecting:
    case wire::Error: return t.red;
    default: return t.textDim;
  }
}

void Terminal::drawOrderbook(Ui& u, Rect r) {
  const Theme& t = theme();
  VenueState* v = feeds.venue(0);

  Rect header{r.x, r.y, r.w, 20};
  u.draw.textAligned(header, "Price (USDT)", t.textDim, DrawList::Left, 10);
  if (v)
    u.draw.textAligned(header, statusText(v->status), statusColor(v->status, t),
                       DrawList::Center);
  u.draw.textAligned(header, "Size (ETH)", t.textDim, DrawList::Right, 10);
  u.draw.rect({r.x, r.y + 20, r.w, 1}, t.border);

  Rect area{r.x, r.y + 21, r.w, r.h - 21};
  if (!v) return;
  const L2Book& b = v->book;
  double bid = b.bestBid(), ask = b.bestAsk();
  if (bid <= 0 || ask <= 0) {
    u.draw.textAligned(area, "syncing…", t.textDim, DrawList::Center);
    return;
  }

  const float rowH = 18.0f, spreadH = 18.0f;
  int nSide = (int)((area.h - spreadH) / rowH) / 2;
  const BookSide& asks = b.asks; // ascending — best ask at [0]
  const BookSide& bids = b.bids; // ascending — best bid at back
  int nAsk = std::min((int)asks.size(), nSide);
  int nBid = std::min((int)bids.size(), nSide);

  double maxSz = 1.0;
  for (int i = 0; i < nAsk; ++i) maxSz = std::max(maxSz, asks.sizes[(size_t)i]);
  for (int i = 0; i < nBid; ++i)
    maxSz = std::max(maxSz, bids.sizes[bids.size() - 1 - (size_t)i]);

  auto row = [&](float y, double price, double size, Color c) {
    float bw = (float)(size / maxSz) * (area.w * 0.45f);
    u.draw.rect({area.x + area.w - bw, y + 2, bw, rowH - 4}, withAlpha(c, 0.15f));
    char pb[32], sb[32];
    snprintf(pb, sizeof(pb), "%.2f", price);
    snprintf(sb, sizeof(sb), "%.3f", size);
    u.draw.textAligned({area.x, y, area.w, rowH}, pb, c, DrawList::Left, 10);
    u.draw.textAligned({area.x, y, area.w, rowH}, sb, t.textDim, DrawList::Right, 10);
  };

  float y = area.y;
  for (int i = nAsk - 1; i >= 0; --i) { // deepest ask at top, best ask at bottom
    row(y, asks.prices[(size_t)i], asks.sizes[(size_t)i], t.red);
    y += rowH;
  }

  char spread[48];
  double sp = ask - bid;
  snprintf(spread, sizeof(spread), "Spread  %.2f  (%.2f bps)", sp, sp / ((ask + bid) * 0.5) * 1e4);
  u.draw.rect({area.x, y, area.w, spreadH}, t.panelAlt);
  u.draw.textAligned({area.x, y, area.w, spreadH}, spread, t.textDim, DrawList::Center);
  y += spreadH;

  for (int i = 0; i < nBid; ++i) {
    size_t idx = bids.size() - 1 - (size_t)i; // best bid first
    row(y, bids.prices[idx], bids.sizes[idx], t.green);
    y += rowH;
  }
}

void Terminal::drawTape(Ui& u, Rect r) {
  const Theme& t = theme();
  VenueState* v = feeds.venue(0);

  Rect header{r.x, r.y, r.w, 20};
  u.draw.textAligned(header, "Time", t.textDim, DrawList::Left, 10);
  u.draw.textAligned({r.x + 90, r.y, 90, 20}, "Price", t.textDim, DrawList::Left);
  if (v)
    u.draw.textAligned(header, statusText(v->status), statusColor(v->status, t),
                       DrawList::Center);
  u.draw.textAligned(header, "Amount", t.textDim, DrawList::Right, 10);
  u.draw.rect({r.x, r.y + 20, r.w, 1}, t.border);

  Rect area{r.x, r.y + 21, r.w, r.h - 21};
  const Tape& tape = feeds.tape;
  if (tape.count == 0) {
    u.draw.textAligned(area, "waiting for trades…", t.textDim, DrawList::Center);
    return;
  }

  const float rowH = 16.0f;
  int rows = std::min((int)(area.h / rowH), (int)tape.count);
  float y = area.y;
  for (int i = 0; i < rows; ++i) {
    const TapeEntry* e = tape.latest((size_t)i);
    if (!e) break;
    Color c = e->side == 0 ? t.green : t.red;

    time_t secs = (time_t)(e->ts / 1000.0);
    struct tm tmv;
    localtime_r(&secs, &tmv);
    char timeBuf[16], price[32], amount[32];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min,
             tmv.tm_sec);
    snprintf(price, sizeof(price), "%.2f", e->price);
    snprintf(amount, sizeof(amount), "%.3f", e->qty);

    u.draw.textAligned({area.x, y, area.w, rowH}, timeBuf, t.textDim, DrawList::Left, 10);
    u.draw.textAligned({area.x + 90, y, 90, rowH}, price, c, DrawList::Left);
    u.draw.textAligned({area.x, y, area.w, rowH}, amount, t.textDim, DrawList::Right, 10);
    y += rowH;
  }
}

void Terminal::drawWatchlist(Ui& u, Rect r) {
  const Theme& t = theme();
  static const char* syms[] = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT",
                               "DOGEUSDT", "ARBUSDT", "LINKUSDT", "AVAXUSDT", "NEARUSDT"};

  Rect header{r.x, r.y, r.w, 20};
  u.draw.textAligned(header, "Symbol", t.textDim, DrawList::Left, 10);
  u.draw.textAligned(header, "Last", t.textDim, DrawList::Right, r.w * 0.35f);
  u.draw.textAligned(header, "24h %", t.textDim, DrawList::Right, 10);
  u.draw.rect({r.x, r.y + 20, r.w, 1}, t.border);

  float rowH = 24.0f;
  for (int i = 0; i < 10; ++i) {
    Rect row{r.x, r.y + 21 + i * rowH, r.w, rowH};
    if (i % 2) u.draw.rect(row, t.panelAlt);

    bool up = frand((uint32_t)i, 11u) > 0.45f;
    Color c = up ? t.green : t.red;

    char price[24], pct[24];
    snprintf(price, sizeof(price), "%.2f",
             100.0 + (double)frand((uint32_t)i, 5u) * 60000.0);
    snprintf(pct, sizeof(pct), "%s%.2f%%", up ? "+" : "-",
             (double)(frand((uint32_t)i, 9u) * 8.0f));

    u.draw.textAligned(row, syms[i], t.text, DrawList::Left, 10);
    u.draw.textAligned(row, price, t.text, DrawList::Right, r.w * 0.35f);
    u.draw.textAligned(row, pct, c, DrawList::Right, 10);
  }
}
