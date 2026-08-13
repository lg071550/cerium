#pragma once

// Normalized per-frame input state, in CSS (logical) pixels relative to the canvas.
// DOM events land between rAF ticks, so callbacks write into this state directly;
// edge flags (pressed/released/...) are cleared by input_end_frame().

struct Input {
  float mouseX = 0, mouseY = 0;
  bool down = false;          // left button held
  bool pressed = false;       // went down since last frame
  bool released = false;      // went up since last frame
  bool dblClick = false;      // double-click since last frame
  float wheelY = 0;           // accumulated vertical wheel delta, px-normalized
  bool escapePressed = false; // Escape keydown since last frame
};

// Register DOM event handlers on the canvas. Call once at startup.
void input_install_hooks();

// Current frame's input state (valid until input_end_frame).
Input& input_frame();

// Clear per-frame edge flags. Call at the very end of each frame.
void input_end_frame();
