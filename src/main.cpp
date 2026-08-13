#include "app/terminal.h"
#include "platform/input.h"
#include "platform/shell.h"
#include "render/renderer.h"
#include "ui/theme.h"

#include <emscripten.h>
#include <emscripten/html5.h>

static Renderer g_renderer;
static Terminal g_terminal;
static bool g_termInited = false;
static double g_last = 0;

static void frame() {
  ShellSize s = shell_sync_canvas();

  // wait for GPU + pipelines + font
  if (!g_renderer.ready("/assets/fonts/IBMPlexMono-Regular.ttf", theme().fontSize,
                        s.dpr))
    return;
  if (!g_termInited) {
    g_terminal.init(&g_renderer.atlas);
    g_renderer.setClearColor(theme().bg.r, theme().bg.g, theme().bg.b);
    g_termInited = true;
  }

  double now = emscripten_performance_now();
  float dt = g_last > 0 ? (float)((now - g_last) / 1000.0) : 0.016f;
  g_last = now;

  Input& in = input_frame();
  g_terminal.frame(in, dt, s.cssW, s.cssH);
  g_renderer.render(g_terminal.ui.draw, s.physW, s.physH, s.dpr);
  input_end_frame();
}

int main() {
  input_install_hooks();
  g_renderer.beginInit("#canvas");
  emscripten_set_main_loop(frame, 0, true);
  return 0;
}
