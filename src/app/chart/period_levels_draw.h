#pragma once

#include "chart_panes.h"
#include "period_levels.h"

void drawPeriodLevels(DrawList& draw, const ChartPane& pane,
                      const PeriodLevels& levels, int mask, bool previous,
                      const std::array<Color, PeriodLevels::Count>& colors,
                      float startBar, float barWidth, float lineWidth, int labelMode);
