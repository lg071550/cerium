#pragma once

#include <webgpu/webgpu.h>

// Owns the WebGPU instance/surface/adapter/device and the surface configuration.
// All Emscripten/webgpu.h version-sensitive API usage lives in wgpu_context.cpp.

struct GpuContext {
  WGPUInstance instance = nullptr;
  WGPUSurface surface = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;
  WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
  // Format each frame is rendered into: the sRGB sibling of surfaceFormat for
  // 8-bit unorm surfaces (so alpha blend composites in linear light), else
  // equal to surfaceFormat. The frame view and all batch pipelines use this.
  WGPUTextureFormat renderFormat = WGPUTextureFormat_Undefined;
  int width = 0, height = 0; // physical px currently configured
  bool ready = false;        // device acquired + surface capabilities known
};

// Creates instance+surface and kicks off async adapter/device acquisition.
void gpu_begin_init(GpuContext& g, const char* canvasSelector);

// Fatal-error channel (boot failures: no adapter/device, font load, ...).
// Default is a no-op beyond stderr diagnostics; the host app installs a
// handler (e.g. splash-screen message) at startup.
using GpuErrorFn = void (*)(const char* msg);
void gpu_set_error_handler(GpuErrorFn fn);
// Reports a fatal error to the installed handler (+ stderr). Shared by the
// gpu and render layers of the library.
void gpu_report_error(const char* msg);

// Call every frame. Returns true once device+queue+surface format are available.
bool gpu_poll(GpuContext& g);

// (Re)configures the surface for a new physical size. No-op if unchanged.
void gpu_resize(GpuContext& g, int physW, int physH);

// Acquires the current surface texture view for rendering this frame.
// Returns nullptr when the surface is temporarily unavailable (e.g. outdated
// after a resize — caller should skip the frame). *outTexture receives the
// texture to release after presenting.
WGPUTextureView gpu_next_frame(GpuContext& g, WGPUTexture* outTexture);

void gpu_present(GpuContext& g);
