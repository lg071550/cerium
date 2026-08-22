#include "widgets.h"
#include "../platform/shell.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

// Per-frame occurrence counter: repeated labels within one id scope still map
// to distinct widget ids (call order must stay stable across frames, as usual
// for immediate-mode ids).
static uint64_t widgetId(Ui& ui, const char* label) {
  static std::unordered_map<uint64_t, int> s_seen;
  static uint32_t s_frame = 0;
  if (ui.frame != s_frame) {
    s_seen.clear();
    s_frame = ui.frame;
  }
  uint64_t h = ui.id(label);
  h ^= (uint64_t)s_seen[h]++ + 0x9e3779b97f4a7c15ull;
  h *= 1099511628211ull;
  return h;
}

Behavior behavior(Ui& ui, Rect r, uint64_t id) {
  Behavior b;
  b.hovered = ui.hovered(r);
  if (b.hovered) ui.hot = id;
  if (b.hovered && ui.input.pressed) ui.active = id;
  if (ui.input.released && ui.active == id) {
    b.clicked = b.hovered;
    ui.active = 0;
  }
  b.held = ui.active == id;
  return b;
}

bool button(Ui& ui, Rect r, const char* label) {
  uint64_t wid = widgetId(ui, label);
  Behavior b = behavior(ui, r, wid);

  const Theme& t = theme();
  Color bg = b.held ? t.accentSoft : (b.hovered ? t.bgHover : t.bgRaised);
  ui.draw.rect(r, bg, 2.0f);
  ui.draw.textAligned(r, label, t.text, DrawList::Center);
  return b.clicked;
}

void listView(Ui& ui, Rect area, int rowCount, float rowH, ListState& state,
              const std::function<void(DrawList&, Rect, int)>& drawRow,
              bool showScrollbar) {
  std::function<void(Ui&, DrawList&, Rect, int)> fwd =
      [&](Ui&, DrawList& d, Rect row, int i) { drawRow(d, row, i); };
  listView(ui, area, rowCount, rowH, state, fwd, showScrollbar);
}

void listView(Ui& ui, Rect area, int rowCount, float rowH, ListState& state,
              const std::function<void(Ui&, DrawList&, Rect, int)>& drawRow,
              bool showScrollbar) {
  const Theme& t = theme();
  float contentH = rowCount * rowH;
  float maxScroll = contentH > area.h ? contentH - area.h : 0.0f;
  bool overflow = maxScroll > 0.0f;

  // scrollbar interaction (thin overlay track on the right edge); id derives
  // from the caller's scope + occurrence, not a global literal
  const float trackW = 8.0f;
  Rect track{area.x + area.w - trackW, area.y, trackW, area.h};
  float thumbH = overflow ? area.h * (area.h / contentH) : 0.0f;
  uint64_t wid = widgetId(ui, "##scrollbar");

  // A release while the scrollbar is hidden (content shrank mid-drag) would
  // otherwise never be observed and wedge wheel scrolling off.
  if (!showScrollbar || !overflow) state.dragging = false;
  if (overflow && showScrollbar) {
    float thumbY0 = track.y + (track.h - thumbH) * (state.scroll / maxScroll);
    Rect thumb{track.x, thumbY0, trackW, thumbH};

    if (state.dragging) {
      ui.active = wid;
      if (!ui.input.down) {
        state.dragging = false;
      } else {
        float dy = ui.input.mouseY - state.dragStartY;
        float scrollRange = track.h - thumbH;
        if (scrollRange > 0) state.scroll = state.dragStartScroll + dy * (maxScroll / scrollRange);
      }
    } else if (ui.input.pressed && track.contains(ui.input.mouseX, ui.input.mouseY)) {
      ui.active = wid;
      if (thumb.contains(ui.input.mouseX, ui.input.mouseY)) {
        state.dragging = true;
        state.dragStartY = ui.input.mouseY;
        state.dragStartScroll = state.scroll;
      } else {
        // track click: page toward the click point
        state.scroll += (ui.input.mouseY < thumb.y ? -1.0f : 1.0f) * area.h * 0.9f;
      }
    }
  }

  if (ui.hovered(area) && !state.dragging && ui.input.wheelY != 0.0f)
    state.scroll += ui.input.wheelY;
  state.scroll = std::clamp(state.scroll, 0.0f, maxScroll);

  ui.draw.pushClip(area);
  int i0 = (int)(state.scroll / rowH);
  int i1 = std::min(rowCount, (int)((state.scroll + area.h) / rowH) + 1);
  for (int i = i0; i < i1; ++i) {
    Rect row{area.x, area.y + i * rowH - state.scroll, area.w, rowH};
    drawRow(ui, ui.draw, row, i);
  }
  ui.draw.popClip();

  if (overflow && showScrollbar) {
    float thumbY = track.y + (track.h - thumbH) * (state.scroll / maxScroll);
    Color c = state.dragging ? t.textDim : t.splitter;
    Rect dr{track.x + 2, std::round(thumbY), trackW - 4, std::round(thumbH)};
    ui.draw.rect(dr, c, 2.0f);
  }
}

bool textField(Ui& ui, Rect r, TextFieldState& st, const char* id,
               const char* placeholder) {
  const Theme& t = theme();
  uint64_t wid = ui.id(id);
  bool hov = ui.hovered(r);
  if (hov) ui.hot = wid;

  if (ui.input.pressed) {
    if (hov && !st.focused) {
      st.focused = true;
      shell_ime_set(st.text.c_str());
      shell_ime_focus(r.x, r.y, r.w, r.h);
      ui.active = wid;
    } else if (!hov && st.focused) {
      st.focused = false;
      shell_ime_blur();
    }
  }
  if (st.focused && ui.focusedField != wid) ui.focusedField = wid;
  if (!st.focused && ui.focusedField == wid) ui.focusedField = 0;

  if (st.focused) {
    for (int i = 0; i < ui.input.keyCount; ++i) {
      const KeyEvent& k = ui.input.keys[i];
      if (!k.down) continue;
      if (k.keyCode == 13) { // Enter
        st.submitted = true;
        st.focused = false;
        shell_ime_blur();
      } else if (k.keyCode == 27) { // Escape
        st.focused = false;
        shell_ime_blur();
      }
    }
  }

  bool changed = false;
  if (st.focused) {
    char buf[512]; // room for pasted values; the old 128 could cut mid-UTF-8
    shell_ime_get(buf, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    if (st.text != buf) {
      st.text = buf;
      changed = true;
    }
  }

  // Recessed field with a tonal focus state. Avoid a neon focus halo—the
  // caret already supplies the precise interaction cue.
  ui.draw.rect(r, st.focused ? t.bgRaised : t.panelAlt, 2.0f);

  Rect inner = r.inset(4);
  ui.draw.pushClip(inner);
  if (st.text.empty() && placeholder && !st.focused) {
    ui.draw.textAligned(inner, placeholder, t.textDim, DrawList::Left, 6);
  } else {
    ui.draw.textAligned(inner, st.text.c_str(), t.text, DrawList::Left, 6);
  }
  if (st.focused) {
    int caret = std::min(shell_ime_caret(), (int)st.text.size());
    std::string prefix = st.text.substr(0, (size_t)caret);
    float cx = inner.x + 6 + ui.draw.measure(prefix.c_str());
    float cy = inner.y + (inner.h - ui.draw.lineHeight()) * 0.5f;
    ui.draw.breakCmd(); // caret quad must layer OVER the field's text
    ui.draw.rect({cx, cy, 1.0f, ui.draw.lineHeight()}, t.accent);
  }
  ui.draw.popClip();
  return changed;
}

bool toggle(Ui& ui, Rect r, const char* label, bool& value) {
  const Theme& t = theme();
  uint64_t wid = ui.id(label);
  Behavior b = behavior(ui, r, wid);

  bool changed = false;
  if (b.clicked) {
    value = !value;
    changed = true;
  }

  float box = r.h - 8;
  Rect br{r.x + 4, r.y + 4, box, box};
  ui.draw.rect(br, value ? t.accent : (b.hovered ? t.bgHover : t.panelAlt), 2.0f);
  if (value) {
    // check mark, drawn in the window-bg tone inset against the accent box
    float cx = br.x, cy = br.y, s = box;
    ui.draw.line(cx + s * 0.22f, cy + s * 0.55f, cx + s * 0.42f, cy + s * 0.75f,
                 t.bg, 1.6f);
    ui.draw.line(cx + s * 0.42f, cy + s * 0.75f, cx + s * 0.80f, cy + s * 0.28f,
                 t.bg, 1.6f);
  }
  ui.draw.textAligned(r, label, t.text, DrawList::Left, box + 14);
  return changed;
}

bool slider(Ui& ui, Rect r, const char* id, float& v, float lo, float hi) {
  const Theme& t = theme();
  uint64_t wid = ui.id(id);
  bool wasActive = ui.active == wid;
  Behavior b = behavior(ui, r, wid);

  // commit-on-release: while held we preview a scratch value keyed by id and
  // only write the output when the drag ends
  static std::unordered_map<uint64_t, float> s_drag;
  bool changed = false;
  float shown = v;
  auto it = s_drag.find(wid);
  if (b.held && ui.input.down) {
    float f = std::clamp((ui.input.mouseX - r.x) / r.w, 0.0f, 1.0f);
    shown = lo + f * (hi - lo);
    s_drag[wid] = shown;
  } else if (it != s_drag.end()) {
    if (!ui.input.down) { // drag ended
      if (wasActive && it->second != v) {
        v = it->second;
        changed = true;
      }
      s_drag.erase(it);
    } else {
      shown = it->second;
    }
  }
  // Evict stale entries when no slider is being dragged. If a slider is
  // removed mid-drag (panel closed), its scratch value lingers forever —
  // this clears all orphans on the first idle frame.
  if (!ui.input.down && !s_drag.empty() && s_drag.size() > 1)
    s_drag.clear();

  float frac = hi > lo ? std::clamp((shown - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
  float cy = r.y + r.h * 0.5f;
  float tx = r.x + 4.0f + frac * (r.w - 8.0f); // thumb stays inside the track
  ui.draw.rect({r.x, cy - 0.5f, r.w, 1.0f}, t.splitter);
  ui.draw.rect({r.x, cy - 0.5f, tx - r.x, 1.0f}, t.accent);
  Color thumb = b.held ? t.accent : (b.hovered ? t.textDim : t.bgHover);
  ui.draw.rect({tx - 3.5f, cy - 4.0f, 7.0f, 8.0f}, thumb, 2.0f);
  return changed;
}

bool chip(Ui& ui, Rect r, const char* label, bool on) {
  const Theme& t = theme();
  uint64_t wid = widgetId(ui, label);
  Behavior b = behavior(ui, r, wid);
  if (on)
    ui.draw.rect(r, t.bgRaised, 1.0f);
  else if (b.hovered || b.held)
    ui.draw.rect(r, b.held ? t.accentSoft : t.bgHover, 1.0f);
  ui.draw.textAligned(r, label, on ? t.text : t.textDim, DrawList::Center);
  return b.clicked;
}

void panelHeader(Ui& ui, Rect r, const char* title, const char* right) {
  const Theme& t = theme();
  ui.draw.textAligned(r, title, t.textDim, DrawList::Left, 10);
  if (right) ui.draw.textAligned(r, right, t.textDim, DrawList::Right, 10);
  ui.draw.rect({r.x, r.y + r.h, r.w, 1}, t.border);
}

void gutterTag(DrawList& d, Rect r, const char* text, Color c) {
  const Theme& t = theme();
  d.shadow(r, 1.0f, 3.0f, 1.0f, hexColor(0x000000, 0.24f));
  d.rect(r, t.bgRaised, 1.0f);
  d.textAligned(r, text, c, DrawList::Center);
}
