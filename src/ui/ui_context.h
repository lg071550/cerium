#pragma once

#include "../platform/input.h"
#include "render/draw_list.h"

#include <cstdint>
#include <vector>

// Immediate-mode UI context: per-frame input snapshot, draw list, hot/active
// tracking, overlay (popover/menu) stack, and tooltip state.
struct Ui {
  Input input{};
  DrawList draw;
  uint64_t hot = 0;    // widget under the mouse
  uint64_t active = 0; // widget currently pressed/dragged
  float dt = 0, time = 0;
  uint32_t frame = 0;

  // ---- overlays (menus/popovers): drawn after panels, window-clipped ----
  struct Overlay {
    uint64_t id;
    Rect rect;
  };
  std::vector<Overlay> overlays;

  // ---- tooltip (set by tip(), drawn by the app at end of frame) ----
  const char* pendingTip = nullptr;
  float tipX = 0, tipY = 0;

  void init(GlyphAtlas* atlas) { draw.setAtlas(atlas); }

  void begin(const Input& in, float dt_) {
    input = in;
    dt = dt_;
    time += dt_;
    frame++;
    hot = 0;
    pendingTip = nullptr;
    draw.reset();
  }

  void end() {
    if (!input.down) active = 0;
  }

  // Call right after begin(): routes escape / outside-press to the top overlay.
  void overlayGate() {
    if (overlays.empty()) return;
    Rect top = overlays.back().rect;
    if (input.escapePressed) {
      overlays.pop_back();
      input.escapePressed = false;
      return;
    }
    bool pressOutside =
        (input.pressed || input.rightPressed) && !top.contains(input.mouseX, input.mouseY);
    if (pressOutside) {
      overlays.pop_back();
      input.pressed = false;
      input.rightPressed = false;
    }
  }

  bool overlayOpen(uint64_t id) const {
    for (auto& o : overlays)
      if (o.id == id) return true;
    return false;
  }

  void openOverlay(uint64_t id, Rect r) {
    if (!overlayOpen(id)) overlays.push_back({id, r});
  }

  void closeOverlay(uint64_t id) {
    for (size_t i = 0; i < overlays.size(); ++i)
      if (overlays[i].id == id) {
        overlays.erase(overlays.begin() + (ptrdiff_t)i);
        return;
      }
  }

  // Hover-delay tooltip: call each frame for hoverable widgets.
  void tip(uint64_t id, Rect r, const char* text) {
    if (hovered(r)) {
      if (m_tipHot != id) {
        m_tipHot = id;
        m_tipSince = time;
      }
      if (time - m_tipSince > 0.4f) {
        pendingTip = text;
        tipX = input.mouseX;
        tipY = input.mouseY;
      }
    } else if (m_tipHot == id) {
      m_tipHot = 0;
    }
  }

  uint64_t id(const char* label) const {
    uint64_t h = m_idStack.empty() ? 1469598103934665603ull : m_idStack.back();
    for (const char* p = label; *p; ++p) {
      h ^= (unsigned char)*p;
      h *= 1099511628211ull;
    }
    return h;
  }

  void pushId(const char* label) { m_idStack.push_back(id(label)); }
  void popId() {
    if (!m_idStack.empty()) m_idStack.pop_back();
  }

  bool hovered(Rect r) const {
    // while an overlay is open, widgets *under* it don't receive hover/clicks;
    // overlay widgets run with inOverlayPass set and bypass this gate
    if (!inOverlayPass && !overlays.empty() &&
        overlays.back().rect.contains(input.mouseX, input.mouseY))
      return false;
    return r.contains(input.mouseX, input.mouseY) &&
           draw.currentClip().contains(input.mouseX, input.mouseY);
  }

  bool inOverlayPass = false; // app sets while drawing overlay widgets

private:
  std::vector<uint64_t> m_idStack;
  uint64_t m_tipHot = 0;
  float m_tipSince = 0;
};
