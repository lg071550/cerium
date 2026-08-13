#include "widgets.h"
#include "theme.h"

#include <algorithm>

bool button(Ui& ui, Rect r, const char* label) {
  uint64_t wid = ui.id(label);
  bool hov = ui.hovered(r);
  if (hov) ui.hot = wid;
  if (hov && ui.input.pressed) ui.active = wid;

  bool clicked = false;
  if (ui.input.released && ui.active == wid) {
    clicked = hov;
    ui.active = 0;
  }

  const Theme& t = theme();
  Color bg = ui.active == wid ? t.accentSoft : (hov ? t.bgHover : t.bgRaised);
  ui.draw.rect(r, bg, t.radius);
  ui.draw.rectOutline(r, t.border, 1.0f, t.radius);
  ui.draw.textAligned(r, label, t.text, DrawList::Center);
  return clicked;
}

void listView(Ui& ui, Rect area, int rowCount, float rowH, ListState& state,
              const std::function<void(DrawList&, Rect, int)>& drawRow) {
  float contentH = rowCount * rowH;
  float maxScroll = contentH > area.h ? contentH - area.h : 0.0f;

  if (ui.hovered(area) && ui.input.wheelY != 0.0f) state.scroll += ui.input.wheelY;
  state.scroll = std::clamp(state.scroll, 0.0f, maxScroll);

  ui.draw.pushClip(area);
  int i0 = (int)(state.scroll / rowH);
  int i1 = std::min(rowCount, (int)((state.scroll + area.h) / rowH) + 1);
  for (int i = i0; i < i1; ++i) {
    Rect row{area.x, area.y + i * rowH - state.scroll, area.w, rowH};
    drawRow(ui.draw, row, i);
  }
  ui.draw.popClip();

  if (maxScroll > 0.0f) {
    const Theme& t = theme();
    float thumbH = area.h * (area.h / contentH);
    float thumbY = area.y + (area.h - thumbH) * (state.scroll / maxScroll);
    ui.draw.rect({area.x + area.w - 3, thumbY, 3, thumbH}, t.splitter, 1.5f);
  }
}
