#include "input.h"
#include "shell.h"

#include <emscripten/html5.h>
#include <cstring>

static Input g_input;

static void push_key(const EmscriptenKeyboardEvent* e, bool down) {
  if (g_input.keyCount >= Input::MAX_KEYS) return; // drop excess (pathological)
  KeyEvent& k = g_input.keys[g_input.keyCount++];
  k.keyCode = e->keyCode;
  k.down = down;
  k.ctrl = e->ctrlKey;
  k.shift = e->shiftKey;
  k.alt = e->altKey;
  strncpy(k.key, e->key, sizeof(k.key) - 1);
  k.key[sizeof(k.key) - 1] = '\0';
  g_input.ctrl = e->ctrlKey;
  g_input.shift = e->shiftKey;
  g_input.alt = e->altKey;
}

static EM_BOOL on_mouse_move(int, const EmscriptenMouseEvent* e, void*) {
  g_input.mouseX = (float)e->targetX;
  g_input.mouseY = (float)e->targetY;
  return true;
}

static EM_BOOL on_mouse_down(int, const EmscriptenMouseEvent* e, void*) {
  g_input.mouseX = (float)e->targetX;
  g_input.mouseY = (float)e->targetY;
  if (e->button == 0) {
    if (!g_input.down) g_input.pressed = true;
    g_input.down = true;
  } else if (e->button == 2) {
    if (!g_input.rightDown) g_input.rightPressed = true;
    g_input.rightDown = true;
  } else if (e->button == 1) {
    g_input.middleDown = true;
  }
  return true;
}

static EM_BOOL on_mouse_up(int, const EmscriptenMouseEvent* e, void*) {
  g_input.mouseX = (float)e->targetX;
  g_input.mouseY = (float)e->targetY;
  if (e->button == 0) {
    if (g_input.down) g_input.released = true;
    g_input.down = false;
  } else if (e->button == 2) {
    if (g_input.rightDown) g_input.rightReleased = true;
    g_input.rightDown = false;
  } else if (e->button == 1) {
    g_input.middleDown = false;
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
  g_input.wheelX += (float)e->deltaX * scale;
  return true; // prevent page scroll
}

static EM_BOOL on_context_menu(int, const EmscriptenMouseEvent*, void*) {
  return true; // suppress the browser context menu — right-click is ours
}

static EM_BOOL on_key_down(int, const EmscriptenKeyboardEvent* e, void*) {
  if (e->keyCode == 27) g_input.escapePressed = true;
  push_key(e, true);
  // claim app shortcuts so the browser doesn't also act on them.
  // note: Ctrl+0 is deliberately NOT claimed — it stays the browser's
  // page-zoom reset (our own UI-scale reset still fires on the same key).
  if (e->keyCode == 27) return true;
  if (e->ctrlKey && (e->keyCode == 187 || e->keyCode == 61 ||   // '='
                     e->keyCode == 189 || e->keyCode == 173))   // '-'
    return true;
  return false;
}

static EM_BOOL on_key_up(int, const EmscriptenKeyboardEvent* e, void*) {
  push_key(e, false);
  return false;
}

void input_install_hooks() {
  const char* canvas = kCanvasSelector;
  emscripten_set_mousemove_callback(canvas, nullptr, false, on_mouse_move);
  emscripten_set_mousedown_callback(canvas, nullptr, false, on_mouse_down);
  // mouseup on window so releasing outside the canvas still ends drags
  emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, on_mouse_up);
  emscripten_set_dblclick_callback(canvas, nullptr, false, on_dbl_click);
  emscripten_set_wheel_callback(canvas, nullptr, false, on_wheel);
  emscripten_set_contextmenu_callback(canvas, nullptr, false, on_context_menu);
  emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, on_key_down);
  emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, on_key_up);
}

Input& input_frame() { return g_input; }

void input_end_frame() {
  g_input.pressed = false;
  g_input.released = false;
  g_input.dblClick = false;
  g_input.rightPressed = false;
  g_input.rightReleased = false;
  g_input.wheelY = 0;
  g_input.wheelX = 0;
  g_input.escapePressed = false;
  g_input.keyCount = 0;
}
