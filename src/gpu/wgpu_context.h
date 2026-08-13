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
  int width = 0, height = 0; // physical px currently configured
  bool ready = false;        // device acquired + surface capabilities known
};

// Creates instance+surface and kicks off async adapter/device acquisition.
void gpu_begin_init(GpuContext& g, const char* canvasSelector);

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
