#include "shell.h"

#include <emscripten/html5.h>
#include <emscripten.h>
#include <cstdlib>
#include <cstring>

EM_JS(char*, storage_get_js, (const char* key), {
  var v = localStorage.getItem(UTF8ToString(key));
  if (v === null) return 0;
  var n = lengthBytesUTF8(v) + 1;
  var p = _malloc(n);
  stringToUTF8(v, p, n);
  return p;
});

EM_JS(void, storage_set_js, (const char* key, const char* value), {
  try {
    localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
  } catch (e) { /* quota / privacy mode — ignore */ }
});

ShellSize shell_sync_canvas() {
  ShellSize s{};
  double w = 0, h = 0;
  emscripten_get_element_css_size("#canvas", &w, &h);
  s.cssW = (float)w;
  s.cssH = (float)h;
  s.dpr = (float)emscripten_get_device_pixel_ratio();
  s.physW = (int)(s.cssW * s.dpr + 0.5f);
  s.physH = (int)(s.cssH * s.dpr + 0.5f);
  if (s.physW < 1) s.physW = 1;
  if (s.physH < 1) s.physH = 1;

  static int curW = 0, curH = 0;
  if (s.physW != curW || s.physH != curH) {
    emscripten_set_canvas_element_size("#canvas", s.physW, s.physH);
    curW = s.physW;
    curH = s.physH;
  }
  return s;
}

EM_JS(void, set_cursor_js, (const char* cursor), {
  document.getElementById('canvas').style.cursor = UTF8ToString(cursor);
});

char* shell_storage_get(const char* key) { return storage_get_js(key); }

void shell_storage_set(const char* key, const char* value) { storage_set_js(key, value); }

void shell_set_cursor(const char* cursor) {
  static char last[32] = "";
  if (strncmp(last, cursor, sizeof(last)) == 0) return;
  strncpy(last, cursor, sizeof(last) - 1);
  last[sizeof(last) - 1] = '\0';
  set_cursor_js(cursor);
}
