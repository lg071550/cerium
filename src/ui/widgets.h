#pragma once

#include "ui_context.h"

#include <functional>

bool button(Ui& ui, Rect r, const char* label);

// Virtualized vertical list: only visible rows are emitted. wheelY scrolls
// when hovered; a minimal scrollbar thumb is drawn on overflow.
struct ListState {
  float scroll = 0;
};

void listView(Ui& ui, Rect area, int rowCount, float rowH, ListState& state,
              const std::function<void(DrawList&, Rect rowRect, int row)>& drawRow);
