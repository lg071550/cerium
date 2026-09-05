#include "drawings.h"

#include "../../platform/shell.h"
#include "../../ui/theme.h"
#include "../price_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr float kHit = 7.0f;
constexpr float kFib[] = {0.0f, 0.236f, 0.382f, 0.5f, 0.618f, 0.786f, 1.0f};
constexpr const char* kFibLbl[] = {"0", "0.236", "0.382", "0.5", "0.618", "0.786", "1"};

int barAtTime(const CandleSeries& cs, double ts) {
  if (cs.v.empty()) return 0;
  int lo = 0, hi = (int)cs.v.size() - 1;
  if (ts <= cs.v.front().ts) return 0;
  if (ts >= cs.v.back().ts) return hi;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (cs.v[(size_t)mid].ts <= ts) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

double timeAtBar(const CandleSeries& cs, int i) {
  if (cs.v.empty()) return 0;
  i = std::clamp(i, 0, (int)cs.v.size() - 1);
  return cs.v[(size_t)i].ts;
}

float xOfBar(const ChartPane& pane, float startF, float bw, int i) {
  return pane.area.x + ((float)i - startF) * bw + bw * 0.5f;
}

int barAtX(const ChartPane& pane, float startF, float bw, int size, float x) {
  if (!(bw > 0)) return 0;
  int i = (int)std::floor(startF + (x - pane.area.x) / bw);
  return std::clamp(i, 0, std::max(0, size - 1));
}

double barVolume(const Candle& c) { return c.aggVol > 0 ? c.aggVol : c.vol; }

float distPtSeg(float px, float py, float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0;
  float len2 = dx * dx + dy * dy;
  float t = 0;
  if (len2 > 1e-6f) t = std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f);
  float x = x0 + t * dx, y = y0 + t * dy;
  return std::hypot(px - x, py - y);
}

void screenOf(const ChartDrawing& g, const ChartPane& pane, const CandleSeries& cs,
              float startF, float bw, float& x0, float& y0, float& x1, float& y1) {
  int b0 = barAtTime(cs, g.t0);
  int b1 = barAtTime(cs, g.t1);
  x0 = xOfBar(pane, startF, bw, b0);
  x1 = xOfBar(pane, startF, bw, b1);
  y0 = pane.yOf(g.p0);
  y1 = pane.yOf(g.p1);
}

int hitHandle(const ChartDrawing& g, const ChartPane& pane, const CandleSeries& cs,
              float startF, float bw, float mx, float my) {
  if (drawClickCommit(g.kind)) return 0;
  float x0, y0, x1, y1;
  screenOf(g, pane, cs, startF, bw, x0, y0, x1, y1);
  if (std::hypot(mx - x0, my - y0) <= kHit + 2.0f) return 1;
  if (std::hypot(mx - x1, my - y1) <= kHit + 2.0f) return 2;
  return 0;
}

float hitDist(const ChartDrawing& g, const ChartPane& pane, const CandleSeries& cs,
              float startF, float bw, float mx, float my) {
  float x0, y0, x1, y1;
  screenOf(g, pane, cs, startF, bw, x0, y0, x1, y1);
  const Rect& a = pane.area;
  switch (g.kind) {
    case DrawTool::HLine:
      return std::fabs(my - y0);
    case DrawTool::Avwap:
      return distPtSeg(mx, my, x0, y0, a.x + a.w, y0);
    case DrawTool::Rect:
    case DrawTool::Fib: {
      float l = std::min(x0, x1), r = std::max(x0, x1);
      float t = std::min(y0, y1), b = std::max(y0, y1);
      float dx = mx < l ? l - mx : mx > r ? mx - r : 0;
      float dy = my < t ? t - my : my > b ? my - b : 0;
      if (dx == 0 && dy == 0)
        return std::min({mx - l, r - mx, my - t, b - my});
      return std::hypot(dx, dy);
    }
    case DrawTool::Ray: {
      float ux = x1 - x0, uy = y1 - y0;
      float len = std::hypot(ux, uy);
      if (len < 1.0f) return std::hypot(mx - x0, my - y0);
      ux /= len;
      uy /= len;
      float t = (mx - x0) * ux + (my - y0) * uy;
      if (t < 0) return std::hypot(mx - x0, my - y0);
      return std::hypot(mx - (x0 + ux * t), my - (y0 + uy * t));
    }
    default:
      return distPtSeg(mx, my, x0, y0, x1, y1);
  }
}

Color drawColor(bool selected, bool hover) {
  const Theme& t = theme();
  return (selected || hover) ? t.text : t.accent;
}

void drawHandle(DrawList& d, float x, float y, Color c) {
  d.rect({x - 3.0f, y - 3.0f, 6.0f, 6.0f}, c);
}

void drawAvwap(DrawList& d, const ChartDrawing& g, const ChartPane& pane,
               const CandleSeries& cs, float startF, float bw, Color c) {
  int a0 = barAtTime(cs, g.t0);
  static thread_local std::vector<float> xy;
  xy.clear();
  double pv = 0, vol = 0;
  for (int i = a0; i < (int)cs.v.size(); ++i) {
    const Candle& bar = cs.v[(size_t)i];
    double tp = (bar.h + bar.l + bar.c) / 3.0;
    double v = barVolume(bar);
    pv += tp * v;
    vol += v;
    if (!(vol > 0)) continue;
    float x = xOfBar(pane, startF, bw, i);
    float y = pane.yOf(pv / vol);
    if (x < pane.area.x - 4.0f) continue;
    if (x > pane.area.x + pane.area.w + 4.0f) break;
    xy.push_back(x);
    xy.push_back(y);
  }
  if (xy.size() >= 4)
    d.polyline(xy.data(), (int)(xy.size() / 2), c, 1.5f);
}

void drawOne(DrawList& d, const ChartDrawing& g, const ChartPane& pane,
             const CandleSeries& cs, float startF, float bw, bool selected,
             bool hover) {
  Color c = drawColor(selected, hover);
  float thick = selected ? 2.0f : 1.0f;
  float x0, y0, x1, y1;
  screenOf(g, pane, cs, startF, bw, x0, y0, x1, y1);
  const Rect& a = pane.area;
  auto handles = [&] {
    if (selected) {
      drawHandle(d, x0, y0, c);
      if (!drawClickCommit(g.kind)) drawHandle(d, x1, y1, c);
    }
  };

  switch (g.kind) {
    case DrawTool::HLine: {
      d.line(a.x, y0, a.x + a.w, y0, c, thick);
      char buf[24];
      formatPrice(g.p0, priceDecimalsForPrice(g.p0), buf, sizeof(buf));
      d.textAligned({a.x + 6.0f, y0 - 14.0f, 80.0f, 14.0f}, buf, c, DrawList::Left);
      return;
    }
    case DrawTool::Avwap:
      drawAvwap(d, g, pane, cs, startF, bw, c);
      d.textAligned({x0 + 4.0f, y0 - 14.0f, 56.0f, 14.0f}, "AVWAP", c,
                    DrawList::Left);
      handles();
      return;
    case DrawTool::Trend:
      d.line(x0, y0, x1, y1, c, thick);
      handles();
      return;
    case DrawTool::Ray: {
      float ux = x1 - x0, uy = y1 - y0;
      float len = std::max(1.0f, std::hypot(ux, uy));
      d.line(x0, y0, x0 + ux / len * 4000.0f, y0 + uy / len * 4000.0f, c, thick);
      handles();
      return;
    }
    case DrawTool::Rect: {
      float l = std::min(x0, x1), r = std::max(x0, x1);
      float top = std::min(y0, y1), bot = std::max(y0, y1);
      Rect box{l, top, std::max(1.0f, r - l), std::max(1.0f, bot - top)};
      d.rect(box, withAlpha(c, 0.08f));
      d.rectOutline(box, c, thick);
      handles();
      return;
    }
    case DrawTool::Fib: {
      float l = std::min(x0, x1), r = std::max(x0, x1);
      float top = std::min(y0, y1), bot = std::max(y0, y1);
      Rect box{l, top, std::max(1.0f, r - l), std::max(1.0f, bot - top)};
      d.rect(box, withAlpha(c, 0.05f));
      d.rectOutline(box, withAlpha(c, 0.85f), 1.0f);
      for (int i = 0; i < 7; ++i) {
        double p = g.p0 + (g.p1 - g.p0) * (double)kFib[i];
        float y = pane.yOf(p);
        if (y < a.y || y > a.y + a.h) continue;
        Color lc = withAlpha(c, i == 0 || i == 6 ? 0.95f : 0.55f);
        d.line(l, y, r, y, lc, i == 0 || i == 3 || i == 6 ? thick : 1.0f);
        char px[24], buf[40];
        formatPrice(p, priceDecimalsForPrice(p), px, sizeof(px));
        snprintf(buf, sizeof(buf), "%s %s", kFibLbl[i], px);
        d.textAligned({r + 4.0f, y - 7.0f, 90.0f, 14.0f}, buf, lc, DrawList::Left);
      }
      handles();
      return;
    }
    default:
      return;
  }
}
} // namespace

void DrawingSet::setStorageKey(const std::string& chartSettingsKey) {
  key = chartSettingsKey;
  auto pos = key.find(".settings.");
  if (pos != std::string::npos) key.replace(pos, 10, ".drawings.");
  else key += ".drawings";
  loaded = false;
}

void DrawingSet::load() {
  if (loaded) return;
  loaded = true;
  items.clear();
  nextId = 1;
  if (key.empty()) return;
  char* saved = shell_storage_get(key.c_str());
  if (!saved) return;
  const char* p = saved;
  int ver = 0;
  if (std::sscanf(p, "%d", &ver) != 1 || ver != 1) {
    std::free(saved);
    return;
  }
  while (*p && *p != '\n') ++p;
  if (*p == '\n') ++p;
  while (*p) {
    ChartDrawing g;
    int kind = 0;
    int n = std::sscanf(p, "%d,%d,%d,%lf,%lf,%lf,%lf", &g.id, &kind, &g.symbol,
                        &g.t0, &g.p0, &g.t1, &g.p1);
    if (n == 7) {
      g.kind = (DrawTool)std::clamp(kind, 1, kDrawToolN - 1);
      items.push_back(g);
      nextId = std::max(nextId, g.id + 1);
    }
    while (*p && *p != '\n') ++p;
    if (*p == '\n') ++p;
  }
  std::free(saved);
}

void DrawingSet::save() const {
  if (key.empty()) return;
  std::string out = "1\n";
  char line[160];
  for (const ChartDrawing& g : items) {
    snprintf(line, sizeof(line), " %d,%d,%d,%.6f,%.10g,%.6f,%.10g\n", g.id,
             (int)g.kind, g.symbol, g.t0, g.p0, g.t1, g.p1);
    out += line;
  }
  shell_storage_set(key.c_str(), out.c_str());
}

const char* DrawingSet::cursor() const {
  if (placing || tool != DrawTool::Pointer) return "crosshair";
  if (dragging) return "grabbing";
  if (hovering) return "pointer";
  return nullptr;
}

const char* DrawingSet::toolLabel() const {
  return kDrawToolNames[std::clamp((int)tool, 0, kDrawToolN - 1)];
}

ChartDrawing* DrawingSet::find(int id) {
  for (ChartDrawing& g : items)
    if (g.id == id) return &g;
  return nullptr;
}

void DrawingSet::commitDraft() {
  draft.id = nextId++;
  items.push_back(draft);
  selectedId = draft.id;
  placing = false;
  save();
}

void DrawingSet::drawPicker(Ui& u) {
  if (!pickerId || !u.overlayOpen(pickerId)) return;
  const Theme& t = theme();
  u.updateOverlayRect(pickerId, pickerRect);
  const Rect& r = pickerRect;
  u.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  u.draw.rect(r, t.panel, t.radius);
  u.draw.rectOutline(r, t.border, 1.0f, t.radius);
  for (int i = 0; i < kDrawToolN; ++i) {
    Rect row{r.x + 4.0f, r.y + 4.0f + (float)i * 22.0f, r.w - 8.0f, 20.0f};
    bool on = (int)tool == i;
    bool hov = u.hovered(row);
    if (on) u.draw.rect(row, t.bgRaised, 1.0f);
    else if (hov) u.draw.rect(row, t.bgHover, 1.0f);
    u.draw.textAligned(row, kDrawToolNames[i], on ? t.accent : t.text,
                       DrawList::Left, 8);
    Behavior b = behavior(u, row, u.id("##draw-tool") + (uint64_t)(i + 1));
    if (b.clicked) {
      tool = (DrawTool)i;
      placing = false;
      u.closeOverlay(pickerId);
      u.input.released = false;
    }
  }
}

bool DrawingSet::handle(Ui& u, const ChartPane& pane, const CandleSeries& cs,
                        int symbol, float startF, float bw, int size) {
  load();
  hovering = false;
  if (cs.v.empty() || size <= 0) return false;

  auto pointAt = [&](float mx, float my, double& ts, double& price) {
    int b = barAtX(pane, startF, bw, size, mx);
    ts = timeAtBar(cs, b);
    price = pane.vOf(my);
  };

  for (int i = 0; i < u.input.keyCount; ++i) {
    const KeyEvent& k = u.input.keys[i];
    if (!k.down) continue;
    if (k.keyCode == 27) {
      if (placing) {
        placing = false;
        return true;
      }
      if (tool != DrawTool::Pointer) {
        tool = DrawTool::Pointer;
        return true;
      }
      if (selectedId) {
        selectedId = 0;
        return true;
      }
    }
    if ((k.keyCode == 46 || k.keyCode == 8) && selectedId && !u.focusedField) {
      items.erase(std::remove_if(items.begin(), items.end(),
                                 [&](const ChartDrawing& g) {
                                   return g.id == selectedId;
                                 }),
                  items.end());
      selectedId = 0;
      save();
      return true;
    }
  }
  auto cancelTool = [&] {
    placing = false;
    tool = DrawTool::Pointer;
  };
  if (u.input.escapePressed && (placing || tool != DrawTool::Pointer)) {
    cancelTool();
    return true;
  }
  if (u.input.rightPressed && pane.area.contains(u.input.mouseX, u.input.mouseY) &&
      (placing || tool != DrawTool::Pointer)) {
    cancelTool();
    u.input.rightPressed = false;
    return true;
  }

  float mx = u.input.mouseX, my = u.input.mouseY;
  bool over = pane.area.contains(mx, my);

  if (dragging && selectedId) {
    ChartDrawing* g = find(selectedId);
    if (!g || !u.input.down) {
      dragging = false;
      save();
      return true;
    }
    double ts, price;
    pointAt(mx, my, ts, price);
    switch (dragHandle) {
      case 1:
        g->t0 = ts;
        g->p0 = price;
        break;
      case 2:
        g->t1 = ts;
        g->p1 = price;
        break;
      default:
        if (drawClickCommit(g->kind)) {
          g->p0 = price;
          if (g->kind == DrawTool::Avwap) g->t0 = ts;
        } else {
          double dP = price - pane.vOf(grabY);
          int dB = barAtX(pane, startF, bw, size, mx) -
                   barAtX(pane, startF, bw, size, grabX);
          auto shiftT = [&](double t) {
            int b = std::clamp(barAtTime(cs, t) + dB, 0, size - 1);
            return timeAtBar(cs, b);
          };
          g->p0 = grabP0 + dP;
          g->p1 = grabP1 + dP;
          g->t0 = shiftT(grabT0);
          g->t1 = shiftT(grabT1);
        }
        break;
    }
    return true;
  }

  if (placing) {
    if (!over && !u.input.down) {
      placing = false;
      return false;
    }
    pointAt(mx, my, draft.t1, draft.p1);
    if (u.input.released) {
      if (drawClickCommit(draft.kind)) {
        draft.t1 = draft.t0;
        draft.p1 = draft.p0;
      }
      commitDraft();
      u.input.released = false;
      return true;
    }
    return true;
  }

  if (tool != DrawTool::Pointer && over && u.input.pressed) {
    double ts, price;
    pointAt(mx, my, ts, price);
    draft = {};
    draft.kind = tool;
    draft.symbol = symbol;
    draft.t0 = draft.t1 = ts;
    draft.p0 = draft.p1 = price;
    placing = true;
    u.input.pressed = false;
    if (drawClickCommit(tool)) commitDraft();
    return true;
  }

  if (!over) return false;

  int hitId = 0;
  float best = kHit;
  for (const ChartDrawing& g : items) {
    if (g.symbol != symbol) continue;
    float dist = hitDist(g, pane, cs, startF, bw, mx, my);
    if (dist < best) {
      best = dist;
      hitId = g.id;
    }
  }
  hovering = hitId != 0;
  if (hitId && u.input.pressed) {
    selectedId = hitId;
    if (ChartDrawing* g = find(hitId)) {
      dragHandle = hitHandle(*g, pane, cs, startF, bw, mx, my);
      dragging = true;
      grabX = mx;
      grabY = my;
      grabP0 = g->p0;
      grabP1 = g->p1;
      grabT0 = g->t0;
      grabT1 = g->t1;
    }
    u.input.pressed = false;
    return true;
  }
  if (u.input.pressed && selectedId) selectedId = 0;
  return false;
}

void DrawingSet::draw(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                      int symbol, float startF, float bw) const {
  if (cs.v.empty()) return;
  for (const ChartDrawing& g : items) {
    if (g.symbol != symbol) continue;
    drawOne(d, g, pane, cs, startF, bw, g.id == selectedId, false);
  }
  if (placing)
    drawOne(d, draft, pane, cs, startF, bw, true, true);
}
