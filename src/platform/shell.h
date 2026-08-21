#pragma once

// DOM selector of the app canvas. index.html owns the element; this is the
// single definition of the selector string — shell/input/main all use it.
inline constexpr const char* kCanvasSelector = "#canvas";

struct ShellSize {
  float cssW, cssH;   // logical size
  float dpr;          // device pixel ratio
  int physW, physH;   // backing store size = css * dpr
};

// Sync canvas backing-store size with its CSS size * devicePixelRatio.
// Returns current sizes; physW/physH are what the GPU surface should use.
ShellSize shell_sync_canvas();

// localStorage access. shell_storage_get returns a malloc'd string (caller
// frees) or nullptr when the key is absent.
char* shell_storage_get(const char* key);
void shell_storage_set(const char* key, const char* value);
void shell_storage_remove(const char* key);

// Sets the OS cursor over the canvas ("default", "pointer", "col-resize",
// "row-resize", "grabbing", ...). Cheap to call every frame — no-ops on repeat.
void shell_set_cursor(const char* cursor);

// Replaces the boot splash text with an error message (fatal init failures).
void shell_boot_error(const char* msg);

// Hidden-DOM-input text editing bridge (IME-compatible). One shared input is
// positioned over the focused field; we render text/caret ourselves in WASM.
void shell_ime_focus(float x, float y, float w, float h); // logical px
void shell_ime_blur();
int shell_ime_get(char* buf, int cap); // current value → buf, returns byte length
void shell_ime_set(const char* value);
int shell_ime_caret(); // selectionStart (byte index)
