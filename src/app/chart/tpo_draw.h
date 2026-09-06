#pragma once
#include "tpo_profile.h"
#include "chart_panes.h"
#include "../../ui/ui_context.h"

void drawTpoProfiles(Ui& u, const ChartPane& pane, const TpoProfiles& profiles,
                     const CandleSeries& candles, float startBar, float barWidth,
                     bool split, int bracketMinutes, int marks, int64_t& selected, int extensions, int labelMask);
