#include "period_levels_draw.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

void drawPeriodLevels(DrawList& draw, const ChartPane& pane,
                      const PeriodLevels& levels, int mask, bool previous,
                      const std::array<Color, PeriodLevels::Count>& colors,
                      float startBar, float barWidth, float lineWidth, int labelMode) {
  struct Label {
    double price;
    float y, labelY, x;
    Color color;
    bool prior;
    char name[96];
  };
  std::array<Label, 6> labels{};
  size_t count = 0;
  const float right = pane.area.x + pane.area.w - 6;
  const float top = pane.area.y + 9, bottom = pane.area.y + pane.area.h - 9;
  if (bottom < top || pane.area.w < 100) return;
  constexpr const char* full[] = {"DAY", "WEEK", "MONTH"};
  constexpr const char* shortNames[] = {"DO", "WO", "MO"};
  const auto* names = labelMode == 1 ? shortNames : full;
  const char* priorPrefix = labelMode == 1 ? "P" : "PREV ";
  const auto add = [&](const PeriodOpen& open, size_t period, bool prior) {
    if (!open.known()) return;
    const float y = pane.yOf(open.price);
    const float x = std::max(pane.area.x,
        pane.area.x + ((float)open.bar - startBar + 0.5f) * barWidth);
    if (!std::isfinite(y) || open.price < pane.lo || open.price > pane.hi || x >= right)
      return;
    // One annotation for coincident opens, retaining every period name.
    for (size_t i = 0; i < count; ++i) {
      Label& label = labels[i];
      if (label.price != open.price) continue;
      const size_t used = std::strlen(label.name);
      snprintf(label.name + used, sizeof(label.name) - used, " / %s%s",
               prior ? priorPrefix : "", names[period]);
      label.x = std::min(label.x, x);
      return;
    }
    Label& label = labels[count++];
    label = {open.price, y, std::clamp(y, top, bottom), x,
             colors[period], prior, {}};
    snprintf(label.name, sizeof(label.name), "%s%s", prior ? priorPrefix : "", names[period]);
  };
  for (size_t i = 0; i < PeriodLevels::Count; ++i)
    if (mask & (1 << i)) add(levels.current[i], i, false);
  if (previous)
    for (size_t i = 0; i < PeriodLevels::Count; ++i)
      if (mask & (1 << i)) add(levels.previous[i], i, true);
  if (!count) return;
  std::sort(labels.begin(), labels.begin() + count,
            [](const Label& a, const Label& b) { return a.y < b.y; });
  // Keep line anchors at the true price; displace only crowded labels.
  const float gap = std::min(17.0f, (bottom - top) / (float)std::max<size_t>(1, count - 1));
  for (size_t i = 1; i < count; ++i)
    labels[i].labelY = std::max(labels[i].labelY, labels[i - 1].labelY + gap);
  labels[count - 1].labelY = std::min(labels[count - 1].labelY, bottom);
  for (size_t i = count - 1; i > 0; --i)
    labels[i - 1].labelY = std::min(labels[i - 1].labelY, labels[i].labelY - gap);
  for (size_t i = 0; i < count; ++i) {
    const Label& label = labels[i];
    draw.linePattern(label.x, label.y, right, label.y,
        withAlpha(label.color, label.prior ? 0.28f : 0.45f), lineWidth,
        label.prior ? DrawList::LineStyle::Dashed : DrawList::LineStyle::Solid);
    if (labelMode != 2 && std::fabs(label.labelY - label.y) > 1)
      draw.linePattern(right, label.y, right + 3, label.labelY,
                       withAlpha(label.color, 0.35f), 1, DrawList::LineStyle::Solid);
  }
  if (labelMode == 2) return;
  draw.breakCmd(); // label backgrounds cover the level lines beneath them
  for (size_t i = 0; i < count; ++i) {
    const Label& label = labels[i];
    char price[24], text[144];
    chartFmtPrice(price, sizeof(price), label.price);
    snprintf(text, sizeof(text), "%s%s  %s", label.name, labelMode == 1 ? "" : " OPEN", price);
    const float width = std::min(draw.measure(text) + 10, pane.area.w - 12);
    const Rect rect{right - width, label.labelY - 8, width, 16};
    draw.rect(rect, theme().panel);
    draw.textFit(rect, text, withAlpha(label.color, label.prior ? 0.6f : 0.9f),
                 DrawList::Right, 4);
  }
}
