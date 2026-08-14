#include "wgpu_context.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// error channel (handler installed by the host app; stderr always)
// ---------------------------------------------------------------------------

static GpuErrorFn g_errorFn = nullptr;

void gpu_set_error_handler(GpuErrorFn fn) { g_errorFn = fn; }

void gpu_report_error(const char* msg) {
  fprintf(stderr, "cerium: %s\n", msg);
  if (g_errorFn) g_errorFn(msg);
}

// Returns the sRGB sibling of an 8-bit unorm surface format so we can render
// through an sRGB view (linear-light blending) while keeping the *surface*
// configured with the non-sRGB format browsers prefer. Other formats pass
// through unchanged (no sRGB reinterpretation).
static WGPUTextureFormat srgb_view_format(WGPUTextureFormat f) {
  switch (f) {
    case WGPUTextureFormat_BGRA8Unorm: return WGPUTextureFormat_BGRA8UnormSrgb;
    case WGPUTextureFormat_RGBA8Unorm: return WGPUTextureFormat_RGBA8UnormSrgb;
    default: return f;
  }
}

// ---------------------------------------------------------------------------
// async acquisition (callbacks fire from the browser event loop / ProcessEvents)
// ---------------------------------------------------------------------------

static void on_error(WGPUDevice const*, WGPUErrorType type, WGPUStringView message,
                     void*, void*) {
  fprintf(stderr, "webgpu error (%d): %.*s\n", (int)type,
          (int)message.length, message.data ? message.data : "");
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView message, void* ud1, void* ud2) {
  GpuContext& g = *static_cast<GpuContext*>(ud1);
  if (status != WGPURequestDeviceStatus_Success) {
    fprintf(stderr, "webgpu: device request failed: %.*s\n",
            (int)message.length, message.data ? message.data : "");
    gpu_report_error("WebGPU device request failed");
    return;
  }
  g.device = device;
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView message, void* ud1, void* ud2) {
  GpuContext& g = *static_cast<GpuContext*>(ud1);
  if (status != WGPURequestAdapterStatus_Success) {
    fprintf(stderr, "webgpu: adapter request failed: %.*s\n",
            (int)message.length, message.data ? message.data : "");
    gpu_report_error("WebGPU is not available in this browser");
    return;
  }
  g.adapter = adapter;

  WGPUDeviceDescriptor desc = {};
  desc.uncapturedErrorCallbackInfo.callback = on_error;
  WGPURequestDeviceCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = on_device;
  cb.userdata1 = &g;
  wgpuAdapterRequestDevice(g.adapter, &desc, cb);
}

void gpu_begin_init(GpuContext& g, const char* canvasSelector) {
  g.instance = wgpuCreateInstance(nullptr);

  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc = {};
  canvasDesc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
  canvasDesc.selector.data = canvasSelector;
  canvasDesc.selector.length = WGPU_STRLEN;

  WGPUSurfaceDescriptor sd = {};
  sd.nextInChain = &canvasDesc.chain;
  g.surface = wgpuInstanceCreateSurface(g.instance, &sd);

  WGPURequestAdapterOptions opts = {};
  opts.compatibleSurface = g.surface;
  opts.powerPreference = WGPUPowerPreference_HighPerformance;

  WGPURequestAdapterCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = on_adapter;
  cb.userdata1 = &g;
  wgpuInstanceRequestAdapter(g.instance, &opts, cb);
}

bool gpu_poll(GpuContext& g) {
  if (g.ready) return true;
  if (!g.instance) return false;
  wgpuInstanceProcessEvents(g.instance);
  if (!g.device || !g.adapter) return false;

  g.queue = wgpuDeviceGetQueue(g.device);

  WGPUSurfaceCapabilities caps = {};
  wgpuSurfaceGetCapabilities(g.surface, g.adapter, &caps);
  // The canvas is color-managed: we output linear-light values and let the
  // sRGB surface encode on write. Prefer BGRA8UnormSrgb.
  g.surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
  for (size_t i = 0; i < caps.formatCount; ++i) {
    if (caps.formats[i] == WGPUTextureFormat_BGRA8UnormSrgb ||
        caps.formats[i] == WGPUTextureFormat_RGBA8UnormSrgb) {
      g.surfaceFormat = caps.formats[i];
      break;
    }
  }
  if (caps.formatCount > 0 && g.surfaceFormat == WGPUTextureFormat_BGRA8Unorm) {
    // no sRGB variant offered — fall back to the first advertised format
    g.surfaceFormat = caps.formats[0];
  }
  wgpuSurfaceCapabilitiesFreeMembers(caps);
  g.renderFormat = srgb_view_format(g.surfaceFormat);

  g.ready = true;
  return true;
}

void gpu_resize(GpuContext& g, int physW, int physH) {
  if (physW == g.width && physH == g.height) return;
  if (physW < 1 || physH < 1) return;
  g.width = physW;
  g.height = physH;

  WGPUSurfaceConfiguration cfg = {};
  cfg.device = g.device;
  cfg.format = g.surfaceFormat;
  cfg.usage = WGPUTextureUsage_RenderAttachment;
  cfg.width = (uint32_t)physW;
  cfg.height = (uint32_t)physH;
  cfg.presentMode = WGPUPresentMode_Fifo;
  cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
  // Declare the sRGB view format so an sRGB view of the swapchain texture is
  // valid; the surface itself stays configured with the non-sRGB format.
  if (g.renderFormat != g.surfaceFormat) {
    cfg.viewFormatCount = 1;
    cfg.viewFormats = &g.renderFormat;
  }
  wgpuSurfaceConfigure(g.surface, &cfg);
}

WGPUTextureView gpu_next_frame(GpuContext& g, WGPUTexture* outTexture) {
  *outTexture = nullptr;

  WGPUSurfaceTexture st = {};
  wgpuSurfaceGetCurrentTexture(g.surface, &st);
  if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
    // outdated/lost mid-resize — reconfigure and skip this frame
    if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
        st.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
      int w = g.width, h = g.height;
      g.width = 0; // force reconfigure
      gpu_resize(g, w, h);
    }
    return nullptr;
  }

  *outTexture = st.texture;
  if (g.renderFormat != g.surfaceFormat) {
    // Render through an sRGB view so alpha blends composite in linear light.
    WGPUTextureViewDescriptor viewDesc = {};
    viewDesc.format = g.renderFormat;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;
    return wgpuTextureCreateView(st.texture, &viewDesc);
  }
  return wgpuTextureCreateView(st.texture, nullptr);
}

// Emscripten presents automatically when the rAF callback returns —
// wgpuSurfacePresent is unsupported and aborts if called.
void gpu_present(GpuContext&) {}
