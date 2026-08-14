#pragma once

// Normalized per-frame input state, in CSS (logical) pixels relative to the canvas.
// DOM events land between rAF ticks, so callbacks write into this state directly;
// edge flags (pressed/released/...) are cleared by input_end_frame().

struct KeyEvent {
  int keyCode = 0;      // DOM keyCode (27=Esc, 13=Enter, 8=Backspace, 46=Del, arrows 37-40)
  char key[8] = {0};    // KeyboardEvent.key, utf8 (truncated)
  bool down = false;    // true = keydown, false = keyup
  bool ctrl = false, shift = false, alt = false;
};

struct Input {
  float mouseX = 0, mouseY = 0;

  bool down = false;          // left button held
  bool pressed = false;       // left went down since last frame
  bool released = false;      // left went up since last frame
  bool dblClick = false;      // left double-click since last frame

  bool rightDown = false;     // right held
  bool rightPressed = false;  // right went down since last frame
  bool rightReleased = false; // right went up since last frame

  bool middleDown = false;

  float wheelY = 0, wheelX = 0; // accumulated wheel deltas, px-normalized

  bool escapePressed = false;
  bool ctrl = false, shift = false, alt = false; // modifier state (latest known)

  static constexpr int MAX_KEYS = 16;
  KeyEvent keys[MAX_KEYS];
  int keyCount = 0; // key events since last frame (oldest dropped on overflow)
};

// Register DOM event handlers on the canvas. Call once at startup.
void input_install_hooks();

// Current frame's input state (valid until input_end_frame).
Input& input_frame();

// Clear per-frame edge flags. Call at the very end of each frame.
void input_end_frame();
