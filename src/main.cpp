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
static double g_lastRender = 0;
static float g_lastMX = -1, g_lastMY = -1;

#ifdef CERIUM_STRESS
// Stress probe (off by default; build with -DCERIUM_STRESS): appends ~10k
// quads + ~50k glyphs per rendered frame to exercise the batchers.
// Deterministic LCG — no <random>.
static uint32_t g_stressLcg = 0x2f6e2b1u;
static float stressRand() {
  g_stressLcg = g_stressLcg * 1664525u + 1013904223u;
  return (float)(g_stressLcg >> 8) / 16777216.0f;
}
#endif

static void frame() {
  ShellSize s = shell_sync_canvas();

  // wait for GPU + pipelines + font
  if (!g_renderer.ready("/assets/fonts/IBMPlexMono-Regular.ttf", theme().fontSize,
                        s.dpr))
    return;
  if (!g_termInited) {
    g_terminal.init(&g_renderer);
    g_renderer.setClearColor(theme().bg.r, theme().bg.g, theme().bg.b);
    g_termInited = true;
  }

  double now = emscripten_performance_now();
  float dt = g_last > 0 ? (float)((now - g_last) / 1000.0) : 0.016f;
  g_last = now;

  Input& in = input_frame();

  // feeds drain every frame regardless of rendering
  int applied = g_terminal.feeds.frame();

  // on-demand rendering: skip the GPU submit when nothing could have changed
  bool inputEdge = in.pressed || in.released || in.dblClick || in.rightPressed ||
                   in.rightReleased || in.wheelY != 0 || in.wheelX != 0 ||
                   in.keyCount > 0 || in.escapePressed;
  bool moved = in.mouseX != g_lastMX || in.mouseY != g_lastMY;
  g_lastMX = in.mouseX;
  g_lastMY = in.mouseY;
  bool heartbeat = (now - g_lastRender) > 250.0; // fps text + time-driven widgets

  if (applied > 0 || inputEdge || moved || heartbeat || g_terminal.drag.active) {
    g_terminal.frame(in, dt, s.cssW, s.cssH);
#ifdef CERIUM_STRESS
    {
      DrawList& dl = g_terminal.ui.draw;
      for (int i = 0; i < 10000; ++i) {
        Color c{stressRand(), stressRand(), stressRand(), 0.4f};
        dl.rect({stressRand() * s.cssW, 48 + stressRand() * (s.cssH - 48),
                 8 + stressRand() * 60, 6 + stressRand() * 30},
                c, 3.0f);
      }
      // 50 glyphs per call → 50k glyph instances
      static const char* kStress =
          "stress glyph run 0123456789 abcdefghijklmnopqrstuvwxyz";
      for (int i = 0; i < 1000; ++i) {
        Color c{stressRand(), stressRand(), stressRand(), 0.8f};
        dl.text(stressRand() * s.cssW, 48 + stressRand() * (s.cssH - 48), kStress, c);
      }
    }
#endif
    g_renderer.render(g_terminal.ui.draw, s.physW, s.physH, s.dpr);
    g_lastRender = now;
  }

  input_end_frame();
}

int main() {
  input_install_hooks();
  gpu_set_error_handler(shell_boot_error); // render/gpu fatal errors → splash
  g_renderer.beginInit("#canvas");
  emscripten_set_main_loop(frame, 0, true);
  return 0;
}
