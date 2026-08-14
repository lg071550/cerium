#include "renderer.h"
#include "draw_list.h"
#include "line_batch.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

// clearR/G/B are authored sRGB; the sRGB surface interprets clearValue as
// linear light, so linearize to keep the background displaying unchanged.
static float srgb_to_linear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void Renderer::beginInit(const char* canvasSelector) {
  gpu_begin_init(m_gpu, canvasSelector);
}

bool Renderer::ready(const char* ttfPath, float fontSize, float dpr) {
  if (m_failed) return false;
  if (!m_inited) {
    if (!gpu_poll(m_gpu)) return false;

    m_uniformLayout = pipeline_uniform_layout(m_gpu.device);
    m_uniformBuf = pipeline_uniform_buffer(m_gpu.device);
    m_uniformGroup = pipeline_uniform_group(m_gpu.device, m_uniformLayout, m_uniformBuf);

    m_quads.init(m_gpu.device, m_gpu.renderFormat, m_uniformLayout);
    m_lines.init(m_gpu.device, m_gpu.renderFormat, m_uniformLayout);

    if (!m_atlas.init(m_gpu.device, ttfPath, fontSize, dpr)) {
      m_failed = true;
      gpu_report_error("could not load the embedded font");
      return false;
    }
    m_text.init(m_gpu.device, m_gpu.renderFormat, m_uniformLayout, m_atlas.textureView(),
                m_atlas.sampler());

    m_dpr = dpr;
    m_fontSize = fontSize;
    m_inited = true;
    return true;
  }

  if (dpr != m_dpr) {
    m_atlas.setDpr(dpr);
    m_text.setAtlas(m_atlas.textureView(), m_atlas.sampler());
    m_dpr = dpr;
  }
  if (fontSize != m_fontSize) {
    m_atlas.setFontSize(fontSize);
    m_text.setAtlas(m_atlas.textureView(), m_atlas.sampler());
    m_fontSize = fontSize;
  }
  return true;
}

void Renderer::setClearColor(float r, float g, float b) {
  m_clearR = r;
  m_clearG = g;
  m_clearB = b;
}

void Renderer::render(DrawList& list, int physW, int physH, float dpr) {
  gpu_resize(m_gpu, physW, physH);

  WGPUTexture frameTex = nullptr;
  WGPUTextureView view = gpu_next_frame(m_gpu, &frameTex);
  if (!view) return; // skip frame (resize race)

  // uploads happen after a successful acquire so skipped frames stay free
  FrameUniforms u{};
  u.screenW = (float)physW;
  u.screenH = (float)physH;
  u.scale = dpr;
  wgpuQueueWriteBuffer(m_gpu.queue, m_uniformBuf, 0, &u, sizeof(u));

  // the atlas texture is recreated when the atlas grows mid-run — re-bind
  if (m_atlas.generation() != m_atlasGen) {
    m_text.setAtlas(m_atlas.textureView(), m_atlas.sampler());
    m_atlasGen = m_atlas.generation();
  }
  m_atlas.flush(m_gpu.queue);
  m_quads.upload(m_gpu.queue, list.quads.data(), (uint32_t)list.quads.size());
  m_text.upload(m_gpu.queue, list.glyphs.data(), (uint32_t)list.glyphs.size());
  m_lines.upload(m_gpu.queue, list.lines.data(), (uint32_t)list.lines.size());

  WGPUCommandEncoderDescriptor encDesc = {};
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(m_gpu.device, &encDesc);

  WGPURenderPassColorAttachment colorAtt = {};
  colorAtt.view = view;
  colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  colorAtt.loadOp = WGPULoadOp_Clear;
  colorAtt.storeOp = WGPUStoreOp_Store;
  colorAtt.clearValue = {srgb_to_linear(m_clearR), srgb_to_linear(m_clearG),
                         srgb_to_linear(m_clearB), 1.0f};

  WGPURenderPassDescriptor passDesc = {};
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &colorAtt;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);

  m_stats = {};
  m_stats.cmds = (int)list.cmds.size();
  m_stats.quads = (int)list.quads.size();
  m_stats.glyphs = (int)list.glyphs.size();
  m_stats.lines = (int)list.lines.size();

  int lastX = -1, lastY = -1, lastW = -1, lastH = -1;
  WGPURenderPipeline lastPipe = nullptr; // bind dedup across cmds
  WGPUBuffer lastVB = nullptr;
  wgpuRenderPassEncoderSetBindGroup(pass, 0, m_uniformGroup, 0, nullptr);
  for (const DrawCmd& cmd : list.cmds) {
    if (cmd.quadN == 0 && cmd.glyphN == 0 && cmd.lineN == 0) continue;
    int x = std::clamp((int)std::round(cmd.clip.x * dpr), 0, physW);
    int y = std::clamp((int)std::round(cmd.clip.y * dpr), 0, physH);
    int w = std::clamp((int)std::round((cmd.clip.x + cmd.clip.w) * dpr), 0, physW) - x;
    int h = std::clamp((int)std::round((cmd.clip.y + cmd.clip.h) * dpr), 0, physH) - y;
    if (w <= 0 || h <= 0) continue;
    if (x != lastX || y != lastY || w != lastW || h != lastH) {
      wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)x, (uint32_t)y, (uint32_t)w,
                                          (uint32_t)h);
      lastX = x;
      lastY = y;
      lastW = w;
      lastH = h;
    }
    if (cmd.quadN) {
      m_quads.draw(pass, cmd.quad0, cmd.quadN, lastPipe, lastVB);
      ++m_stats.drawCalls;
    }
    if (cmd.lineN) {
      m_lines.draw(pass, cmd.line0, cmd.lineN, lastPipe, lastVB);
      ++m_stats.drawCalls;
    }
    if (cmd.glyphN) {
      m_text.draw(pass, cmd.glyph0, cmd.glyphN, lastPipe, lastVB);
      ++m_stats.drawCalls;
    }
  }

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBufferDescriptor cmdDesc = {};
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmdDesc);
  wgpuCommandEncoderRelease(encoder);

  wgpuQueueSubmit(m_gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);

  gpu_present(m_gpu);
  wgpuTextureViewRelease(view);
  wgpuTextureRelease(frameTex);
}
