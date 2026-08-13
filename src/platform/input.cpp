#include "input.h"

#include <emscripten/html5.h>

static Input g_input;

static EM_BOOL on_mouse_move(int, const EmscriptenMouseEvent* e, void*) {
  g_input.mouseX = (float)e->targetX;
  g_input.mouseY = (float)e->targetY;
  return true;
}

static EM_BOOL on_mouse_down(int, const EmscriptenMouseEvent* e, void*) {
  if (e->button == 0) {
    g_input.mouseX = (float)e->targetX;
    g_input.mouseY = (float)e->targetY;
    if (!g_input.down) g_input.pressed = true;
    g_input.down = true;
  }
  return true;
}

static EM_BOOL on_mouse_up(int, const EmscriptenMouseEvent* e, void*) {
  if (e->button == 0) {
    g_input.mouseX = (float)e->targetX;
    g_input.mouseY = (float)e->targetY;
    if (g_input.down) g_input.released = true;
    g_input.down = false;
  }
  return true;
}

static EM_BOOL on_dbl_click(int, const EmscriptenMouseEvent* e, void*) {
  if (e->button == 0) g_input.dblClick = true;
  return true;
}

static EM_BOOL on_wheel(int, const EmscriptenWheelEvent* e, void*) {
  // deltaMode: 0 = pixels, 1 = lines, 2 = pages. Normalize to px-ish units.
  float scale = e->deltaMode == 0 ? 1.0f : (e->deltaMode == 1 ? 16.0f : 400.0f);
  g_input.wheelY += (float)e->deltaY * scale;
  return true; // prevent page scroll
}

static EM_BOOL on_key_down(int, const EmscriptenKeyboardEvent* e, void*) {
  if (e->keyCode == 27) { // Escape
    g_input.escapePressed = true;
    return true;
  }
  return false;
}

void input_install_hooks() {
  const char* canvas = "#canvas";
  emscripten_set_mousemove_callback(canvas, nullptr, false, on_mouse_move);
  emscripten_set_mousedown_callback(canvas, nullptr, false, on_mouse_down);
  // mouseup on window so releasing outside the canvas still ends drags
  emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, on_mouse_up);
  emscripten_set_dblclick_callback(canvas, nullptr, false, on_dbl_click);
  emscripten_set_wheel_callback(canvas, nullptr, false, on_wheel);
  emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, on_key_down);
}

Input& input_frame() { return g_input; }

void input_end_frame() {
  g_input.pressed = false;
  g_input.released = false;
  g_input.dblClick = false;
  g_input.wheelY = 0;
  g_input.escapePressed = false;
}
