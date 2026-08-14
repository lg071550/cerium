#pragma once

#include "ui_context.h"

#include <functional>
#include <string>

// Canonical interaction primitive for press/click widgets. hover sets hot,
// pressing inside sets active, clicked = released while active AND still
// hovered; active is cleared on release either way. held = active == id
// (false again on the release frame, matching the old hand-rolled idiom).
struct Behavior {
  bool hovered = false, held = false, clicked = false;
};

Behavior behavior(Ui& ui, Rect r, uint64_t id);

bool button(Ui& ui, Rect r, const char* label);

// Virtualized vertical list: only visible rows are emitted. wheelY scrolls
// when hovered; the scrollbar thumb is draggable and the track pages.
struct ListState {
  float scroll = 0;
  bool dragging = false;
  float dragStartY = 0;
  float dragStartScroll = 0;
};

void listView(Ui& ui, Rect area, int rowCount, float rowH, ListState& state,
              const std::function<void(DrawList&, Rect rowRect, int row)>& drawRow);

// Row callback also receives the Ui so rows can host interactive widgets
// without capturing ui (push your own id scope per row). The overload above
// delegates to this one.
void listView(Ui& ui, Rect area, int rowCount, float rowH, ListState& state,
              const std::function<void(Ui&, DrawList&, Rect rowRect, int row)>& drawRow);

// Text field backed by a hidden DOM input (IME-compatible). We render the
// text and caret ourselves; the DOM element carries editing/IME semantics.
// Returns true when the text changed this frame. Enter sets submitted=true
// (caller clears); Escape cancels focus.
struct TextFieldState {
  std::string text;
  bool focused = false;
  bool submitted = false;
};

bool textField(Ui& ui, Rect r, TextFieldState& state, const char* id,
               const char* placeholder = nullptr);

// Checkbox row: box + label; toggles value on click. Returns true on change.
bool toggle(Ui& ui, Rect r, const char* label, bool& value);

// Horizontal slider; drag thumb or click track. Commit-on-release: the value
// is previewed while dragging and written back only when the drag ends.
// Returns true on commit.
bool slider(Ui& ui, Rect r, const char* id, float& v, float lo, float hi);

// Toggle-style chip: accentSoft when on, bgHover when hovered, panelAlt
// otherwise. Returns true on click.
bool chip(Ui& ui, Rect r, const char* label, bool on);

// Panel header row: title left in textDim (pad 10), optional right-aligned
// text (pad 10), 1px border separator line beneath it.
void panelHeader(Ui& ui, Rect r, const char* title, const char* right = nullptr);

// Chart gutter tag: soft shadow, bgRaised rounded rect (3px), centered text.
void gutterTag(DrawList& d, Rect r, const char* text, Color c);

