#include "app/terminal.h"
#include "platform/input.h"
#include "platform/shell.h"
#include "render/renderer.h"
#include "ui/theme.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>

static Renderer g_renderer;
static Terminal g_terminal;
static bool g_termInited = false;
static double g_last = 0;
static double g_lastRender = 0;
static float g_lastMX = -1, g_lastMY = -1;

// --- perf window (1s rolling): CPU-side frame cost attribution -------------
static double g_accFeed = 0, g_accUi = 0, g_accRender = 0;
static int g_winRaf = 0, g_winRendered = 0, g_winEvents = 0;
static double g_winStart = 0;
// last completed window (read via cerium_perf)
static double g_pFeedMs, g_pUiMs, g_pRenderMs, g_pRafFps, g_pFps, g_pEvents;

// index: 0 feed drain ms  1 ui build ms  2 render submit ms  3 rAF fps
//        4 rendered fps   5 events/frame 6 draw calls  7 quads  8 glyphs
//        9 lines         10 ob ladder levels 11 candles 12 atlas height
//        13 retained chart order-flow prints
extern "C" EMSCRIPTEN_KEEPALIVE double cerium_perf(int i) {
  switch (i) {
    case 0: return g_pFeedMs;
    case 1: return g_pUiMs;
    case 2: return g_pRenderMs;
    case 3: return g_pRafFps;
    case 4: return g_pFps;
    case 5: return g_pEvents;
    case 6: return (double)g_renderer.stats().drawCalls;
    case 7: return (double)g_renderer.stats().quads;
    case 8: return (double)g_renderer.stats().glyphs;
    case 9: return (double)g_renderer.stats().lines;
    case 10: return (double)g_terminal.debugLadderLevels();
    case 11: return (double)g_terminal.feeds.candles.v.size();
    case 12: return (double)g_renderer.atlas()->height();
    case 13: return (double)g_terminal.feeds.orderFlow.v.size();
    case 14: return (double)g_terminal.feeds.candles.tf.kind;
    case 15: return g_terminal.feeds.candles.tf.value;
    default: return 0;
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE double cerium_set_tf(int kind, double value) {
  if (kind < 0 || kind > 2) return 0; // probe-only entry; keep kinds in range
  g_terminal.feeds.setTimeframe({(Timeframe::Kind)(uint8_t)kind, value});
  return g_terminal.feeds.candles.tf.value;
}

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

static bool frame() {
  ShellSize s = shell_sync_canvas();

  // wait for GPU + pipelines + font
  if (!g_renderer.ready("/assets/fonts/IBMPlexMono-Regular.ttf", theme().fontSize,
                        s.dpr))
    return false;
  if (!g_termInited) {
    g_terminal.init(&g_renderer);
    // second face for emphasis (headers/values) — shares the atlas texture;
    // a missing file simply degrades to face 0 at draw time
    g_renderer.atlas()->addFace("/assets/fonts/IBMPlexMono-SemiBold.ttf");
    g_renderer.setClearColor(theme().bg.r, theme().bg.g, theme().bg.b);
    // software-adapter fast path (Renderer::Mode::Auto): pair the renderer's
    // raw-view/blend mode with the matching DrawList quality so SwiftShader
    // sessions get the reduced-coverage path too (hardware stays Full)
    g_terminal.ui.draw.setQuality(g_renderer.fastPath() ? DrawList::Quality::Fast
                                                        : DrawList::Quality::Full);
    g_termInited = true;
    shell_boot_ready();
  }

  double now = emscripten_performance_now();
  float dt = g_last > 0 ? (float)((now - g_last) / 1000.0) : 0.016f;
  g_last = now;

  Input& in = input_frame();

  // feeds drain every frame regardless of rendering
  double tFeed0 = now;
  int applied = g_terminal.feeds.frame();
  double tFeed1 = emscripten_performance_now();

  // on-demand rendering: skip the GPU submit when nothing could have changed
  bool inputEdge = in.pressed || in.released || in.dblClick || in.rightPressed ||
                   in.rightReleased || in.wheelY != 0 || in.wheelX != 0 ||
                   in.keyCount > 0 || in.escapePressed;
  bool moved = in.mouseX != g_lastMX || in.mouseY != g_lastMY;
  g_lastMX = in.mouseX;
  g_lastMY = in.mouseY;
  bool heartbeat = (now - g_lastRender) > 250.0; // fps text + time-driven widgets

  bool busy = applied > 0 || inputEdge || moved || heartbeat ||
             g_terminal.drag.active;
  if (busy) {
    double tUi0 = emscripten_performance_now();
    float uiDt = g_lastRender > 0 ? (float)((now - g_lastRender) / 1000.0) : dt;
    g_terminal.frame(in, dt, uiDt, s.cssW, s.cssH);
    double tUi1 = emscripten_performance_now();
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
    double tR1 = emscripten_performance_now();
    g_accFeed += tFeed1 - tFeed0;
    g_accUi += tUi1 - tUi0;
    g_accRender += tR1 - tUi1;
    ++g_winRendered;
    g_winEvents += applied;
    g_lastRender = now;
  }
  ++g_winRaf;
  if (g_winStart <= 0) g_winStart = now;
  if (now - g_winStart >= 1000.0) { // close the 1s window
    double sec = (now - g_winStart) / 1000.0;
    g_pFeedMs = g_accFeed / std::max(1, g_winRendered);
    g_pUiMs = g_accUi / std::max(1, g_winRendered);
    g_pRenderMs = g_accRender / std::max(1, g_winRendered);
    g_pRafFps = g_winRaf / sec;
    g_pFps = g_winRendered / sec;
    g_pEvents = (double)g_winEvents / std::max(1, g_winRendered);
    g_accFeed = g_accUi = g_accRender = 0;
    g_winRaf = g_winRendered = g_winEvents = 0;
    g_winStart = now;
  }

  input_end_frame();
  return busy;
}

// Dawn's browser surface presents implicitly when the requestAnimationFrame
// callback returns. GPU submits from MessageChannel/timer tasks never reach
// the canvas, so the render loop must remain rAF-owned.
static void frameTick() { (void)frame(); }

int main() {
  input_install_hooks();
  gpu_set_error_handler(shell_boot_error); // render/gpu fatal errors → splash
  g_renderer.beginInit(kCanvasSelector);
  emscripten_set_main_loop(frameTick, 0, 1);
  return 0;
}
