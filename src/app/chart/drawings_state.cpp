#include "drawings.h"
#include "drawing_geometry.h"
#include "../../platform/shell.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <climits>
using namespace drawing_geometry;

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
    if (n == 7 && g.id > 0 && g.id < INT_MAX && kind > 0 && kind < kDrawToolN &&
        g.symbol >= 0 && std::isfinite(g.t0) && std::isfinite(g.t1) &&
        g.t0 > 0 && g.t1 > 0 && g.t0 <= 253402300799999.0 && g.t1 <= 253402300799999.0 &&
        std::isfinite(g.p0) && std::isfinite(g.p1) && g.p0 > 0 && g.p1 > 0 && !find(g.id)) {
      g.kind = (DrawTool)kind;
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


ChartDrawing* DrawingSet::find(int id) {
  for (ChartDrawing& g : items)
    if (g.id == id) return &g;
  return nullptr;
}

void DrawingSet::commitDraft() {
  draft.id = nextId++;
  items.push_back(draft);
  selectedId = draft.id;
  placing = awaitingEnd = false;
  tool = DrawTool::Pointer;
  save();
}


bool DrawingSet::handle(Ui& u, const ChartPane& pane, const CandleSeries& cs,
                        int symbol, float startF, float bw, int size, Rect controls) {
  load();
  currentSymbol = symbol;
  hovering = false;
  if (cs.v.empty() || size <= 0) return false;

  auto pointAt = [&](float mx, float my, double& ts, double& price) {
    int b = barAtX(pane, startF, bw, size, mx);
    ts = timeAtBar(cs, b);
    price = pane.vOf(my);
  };

  // Chart chrome and popups own input before drawing hit-testing. Existing
  // gestures may finish outside the plot, but cannot start on a control.
  if (!u.overlays.empty() || u.focusedField) {
    placing = false;
    if (dragging) { dragging = false; save(); }
    return false;
  }
  const bool overPlot = u.hovered(pane.area) && !controls.contains(u.input.mouseX, u.input.mouseY);
  if (!overPlot && !placing && !dragging) return false;
  auto cancel = [&] {
    if (dragging) if (auto* g = find(selectedId)) {
      g->p0 = grabP0; g->p1 = grabP1; g->t0 = grabT0; g->t1 = grabT1;
    }
    dragging = placing = awaitingEnd = false;
    tool = DrawTool::Pointer;
  };
  for (int i = 0; i < u.input.keyCount; ++i) {
    KeyEvent& k = u.input.keys[i];
    if (!k.down) continue;
    if (k.keyCode == 27) {
      cancel(); selectedId = 0; k.down = false;
      u.input.escapePressed = false;
      return true;
    }
    if ((k.keyCode == 46 || k.keyCode == 8) && selectedId) {
      items.erase(std::remove_if(items.begin(), items.end(),
          [&](const ChartDrawing& g) { return g.id == selectedId; }), items.end());
      selectedId = 0; dragging = placing = false; k.down = false;
      save(); return true;
    }
  }
  if (u.input.escapePressed && armed()) {
    cancel(); u.input.escapePressed = false; return true;
  }
  if (u.input.rightPressed && overPlot && armed()) {
    cancel(); u.input.rightPressed = false; return true;
  }

  float mx = u.input.mouseX, my = u.input.mouseY;
  bool over = overPlot;

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
    if (!over && !u.input.down && !awaitingEnd) {
      placing = false;
      return false;
    }
    pointAt(mx, my, draft.t1, draft.p1);
    if (awaitingEnd && over && u.input.pressed) {
      commitDraft(); u.input.pressed = false; return true;
    }
    if (u.input.released && !awaitingEnd) {
      if (!drawClickCommit(draft.kind) && std::hypot(mx-grabX,my-grabY)<3.0f) {
        awaitingEnd=true; u.input.released=false; return true;
      }
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
    placing = true; awaitingEnd = false;
    grabX=mx; grabY=my;
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

