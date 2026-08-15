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
  emscripten_get_element_css_size(kCanvasSelector, &w, &h);
  s.cssW = (float)w;
  s.cssH = (float)h;
  s.dpr = (float)emscripten_get_device_pixel_ratio();
  s.physW = (int)(s.cssW * s.dpr + 0.5f);
  s.physH = (int)(s.cssH * s.dpr + 0.5f);
  if (s.physW < 1) s.physW = 1;
  if (s.physH < 1) s.physH = 1;

  static int curW = 0, curH = 0;
  if (s.physW != curW || s.physH != curH) {
    emscripten_set_canvas_element_size(kCanvasSelector, s.physW, s.physH);
    curW = s.physW;
    curH = s.physH;
  }
  return s;
}

EM_JS(void, set_cursor_js, (const char* cursor), {
  document.getElementById('canvas').style.cursor = UTF8ToString(cursor);
});

EM_JS(void, boot_error_js, (const char* msg), {
  var el = document.getElementById('loading');
  if (el) {
    el.textContent = 'cerium failed to start: ' + UTF8ToString(msg);
    el.style.color = '#f6465d';
  }
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

void shell_boot_error(const char* msg) { boot_error_js(msg); }

// ---------------------------------------------------------------------------
// IME bridge (hidden DOM input used for text fields)
// ---------------------------------------------------------------------------

EM_JS(void, ime_focus_js, (float x, float y, float w, float h), {
  var el = document.getElementById('ime');
  if (!el) {
    el = document.createElement('input');
    el.id = 'ime';
    el.type = 'text';
    el.autocomplete = 'off';
    el.spellcheck = false;
    el.style.cssText = 'position:fixed;z-index:10;opacity:0.01;background:transparent;' +
                       'color:transparent;border:none;outline:none;caret-color:transparent;' +
                       'font-size:12px;padding:0;margin:0;left:0;top:0;';
    document.body.appendChild(el);
  }
  // park/unpark: blur() hides the input so it can't swallow canvas clicks
  el.style.display = 'block';
  el.style.left = x + 'px';
  el.style.top = y + 'px';
  el.style.width = w + 'px';
  el.style.height = h + 'px';
  el.focus();
});

EM_JS(void, ime_blur_js, (), {
  var el = document.getElementById('ime');
  if (el) {
    el.blur();
    el.style.display = 'none';
  }
});

EM_JS(int, ime_get_js, (char* buf, int cap), {
  var el = document.getElementById('ime');
  if (!el) return 0;
  var v = el.value;
  stringToUTF8(v, buf, cap);
  return lengthBytesUTF8(v) < cap ? lengthBytesUTF8(v) : cap - 1;
});

EM_JS(void, ime_set_js, (const char* value), {
  var el = document.getElementById('ime');
  if (el) el.value = UTF8ToString(value);
});

EM_JS(int, ime_caret_js, (), {
  var el = document.getElementById('ime');
  return el && el.selectionStart != null ? el.selectionStart : 0;
});

void shell_ime_focus(float x, float y, float w, float h) { ime_focus_js(x, y, w, h); }
void shell_ime_blur() { ime_blur_js(); }
int shell_ime_get(char* buf, int cap) { return ime_get_js(buf, cap); }
void shell_ime_set(const char* value) { ime_set_js(value); }
int shell_ime_caret() { return ime_caret_js(); }
