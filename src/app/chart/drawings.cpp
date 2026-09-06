#include "drawings.h"
#include "drawing_geometry.h"

#include "../../platform/shell.h"
#include "../../ui/theme.h"
#include "../price_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace drawing_geometry;
namespace {
Color drawColor(bool selected, bool hover) {
  const Theme& t = theme();
  return (selected || hover) ? t.text : t.accent;
}

void drawHandle(DrawList& d, float x, float y, Color c) {
  d.rect({x - 3.0f, y - 3.0f, 6.0f, 6.0f}, c);
}

void drawAvwap(DrawList& d, const ChartDrawing& g, const ChartPane& pane,
               const CandleSeries& cs, float startF, float bw, Color c) {
  static thread_local std::vector<float> xy;
  avwapPoints(g, pane, cs, startF, bw, xy);
  if (xy.size() >= 4) d.polyline(xy.data(), (int)(xy.size()/2), c, 1.5f);
  if (!xy.empty()) {
    d.textAligned({xy[0]+4,xy[1]-14,56,14}, "AVWAP", c, DrawList::Left);
  }
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

const char* DrawingSet::cursor() const {
  if (placing || tool != DrawTool::Pointer) return "crosshair";
  if (dragging) return "grabbing";
  if (hovering) return "pointer";
  return nullptr;
}

const char* DrawingSet::toolLabel() const {
  return kDrawToolNames[std::clamp((int)tool, 0, kDrawToolN - 1)];
}

void DrawingSet::drawPicker(Ui& u) {
  load();
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
      placing = awaitingEnd = dragging = false;
      u.closeOverlay(pickerId);
      u.input.released = false;
    }
  }
  const float y=r.y+4+kDrawToolN*22;
  u.draw.textAligned({r.x+10,y,r.w-20,20},"DRAWINGS / CURRENT SYMBOL",t.textDim,DrawList::Left);
  if (chip(u,{r.x+6,y+22,(r.w-16)*.5f,22},"DELETE SELECTED",false)) {
    items.erase(std::remove_if(items.begin(),items.end(),[&](const ChartDrawing& g){return g.symbol==currentSymbol && g.id==selectedId;}),items.end());
    selectedId=0; placing=dragging=awaitingEnd=false; tool=DrawTool::Pointer; save();
  }
  if (chip(u,{r.x+r.w*.5f+2,y+22,(r.w-16)*.5f,22},"CLEAR SYMBOL",false)) {
    items.erase(std::remove_if(items.begin(),items.end(),[&](const ChartDrawing& g){return g.symbol==currentSymbol;}),items.end());
    selectedId=0; placing=dragging=awaitingEnd=false; tool=DrawTool::Pointer; save();
  }
  std::vector<int> ids;
  for (auto it=items.rbegin();it!=items.rend();++it) if(it->symbol==currentSymbol)ids.push_back(it->id);
  Rect list{r.x+4,y+48,r.w-8,std::max(0.0f,r.y+r.h-y-52)};
  if(ids.empty()) u.draw.textAligned(list,"No drawings for this symbol",t.textDim,DrawList::Center);
  listView(u,list,(int)ids.size(),24,managerList,[&](Ui& rowUi,DrawList& d,Rect row,int index){
    auto* g=find(ids[index]); if(!g)return;
    char label[64];snprintf(label,sizeof(label),"%s #%d",kDrawToolNames[(int)g->kind],g->id);
    rowUi.pushId(label);
    Rect select{row.x,row.y,std::max(0.0f,row.w-42),22};
    if(chip(rowUi,select,label,g->id==selectedId)) {selectedId=g->id;tool=DrawTool::Pointer;placing=dragging=awaitingEnd=false;}
    if(chip(rowUi,{row.x+row.w-40,row.y,38,22},"DEL",false)) {
      const int id=g->id;
      items.erase(std::remove_if(items.begin(),items.end(),[&](const ChartDrawing& item){return item.id==id;}),items.end());
      if(selectedId==id)selectedId=0;
      save();
    }
    rowUi.popId();
  },false);

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
