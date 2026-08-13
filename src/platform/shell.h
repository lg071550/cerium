#pragma once

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

// Sets the OS cursor over the canvas ("default", "pointer", "col-resize",
// "row-resize", "grabbing", ...). Cheap to call every frame — no-ops on repeat.
void shell_set_cursor(const char* cursor);
