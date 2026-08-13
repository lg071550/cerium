#pragma once

#include "../platform/input.h"
#include "draw_list.h"

#include <cstdint>
#include <vector>

// Immediate-mode UI context: per-frame input snapshot, draw list, and the
// hot/active widget id tracking that drives interaction.
struct Ui {
  Input input{};
  DrawList draw;
  uint64_t hot = 0;    // widget under the mouse
  uint64_t active = 0; // widget currently pressed/dragged
  float dt = 0;
  float time = 0;
  uint32_t frame = 0;

  void init(GlyphAtlas* atlas) { draw.setAtlas(atlas); }

  void begin(const Input& in, float dt_) {
    input = in;
    dt = dt_;
    time += dt_;
    frame++;
    hot = 0;
    draw.reset();
  }

  void end() {
    if (!input.down) active = 0;
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
    return r.contains(input.mouseX, input.mouseY) &&
           draw.currentClip().contains(input.mouseX, input.mouseY);
  }

private:
  std::vector<uint64_t> m_idStack;
};
