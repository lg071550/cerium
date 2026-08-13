#include "renderer.h"
#include "../ui/draw_list.h"
#include "pipeline.h"

#include <algorithm>

void Renderer::beginInit(const char* canvasSelector) { gpu_begin_init(gpu, canvasSelector); }

bool Renderer::ready(const char* ttfPath, float fontSize, float dpr) {
  if (!inited) {
    if (!gpu_poll(gpu)) return false;

    uniformLayout = pipeline_uniform_layout(gpu.device);
    uniformBuf = pipeline_uniform_buffer(gpu.device);
    uniformGroup = pipeline_uniform_group(gpu.device, uniformLayout, uniformBuf);

    quads.init(gpu.device, gpu.surfaceFormat, uniformLayout);

    if (!atlas.init(gpu.device, ttfPath, fontSize, dpr)) return false;
    text.init(gpu.device, gpu.surfaceFormat, uniformLayout, atlas.textureView(),
              atlas.sampler());

    this->dpr = dpr;
    inited = true;
    return true;
  }

  if (dpr != this->dpr) {
    atlas.setDpr(dpr);
    text.setAtlas(atlas.textureView(), atlas.sampler());
    this->dpr = dpr;
  }
  return true;
}

void Renderer::setClearColor(float r, float g, float b) {
  clearR = r;
  clearG = g;
  clearB = b;
}

void Renderer::render(DrawList& list, int physW, int physH, float dpr) {
  gpu_resize(gpu, physW, physH);

  FrameUniforms u{};
  u.screenW = (float)physW;
  u.screenH = (float)physH;
  u.scale = dpr;
  wgpuQueueWriteBuffer(gpu.queue, uniformBuf, 0, &u, sizeof(u));

  atlas.flush(gpu.queue);
  quads.upload(gpu.queue, list.quads.data(), (uint32_t)list.quads.size());
  text.upload(gpu.queue, list.glyphs.data(), (uint32_t)list.glyphs.size());

  WGPUTexture frameTex = nullptr;
  WGPUTextureView view = gpu_next_frame(gpu, &frameTex);
  if (!view) return; // skip frame (resize race)

  WGPUCommandEncoderDescriptor encDesc = {};
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(gpu.device, &encDesc);

  WGPURenderPassColorAttachment colorAtt = {};
  colorAtt.view = view;
  colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  colorAtt.loadOp = WGPULoadOp_Clear;
  colorAtt.storeOp = WGPUStoreOp_Store;
  colorAtt.clearValue = {clearR, clearG, clearB, 1.0};

  WGPURenderPassDescriptor passDesc = {};
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &colorAtt;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);

  for (const DrawCmd& cmd : list.cmds) {
    int x = std::clamp((int)(cmd.clip.x * dpr), 0, physW);
    int y = std::clamp((int)(cmd.clip.y * dpr), 0, physH);
    int w = std::clamp((int)((cmd.clip.x + cmd.clip.w) * dpr), 0, physW) - x;
    int h = std::clamp((int)((cmd.clip.y + cmd.clip.h) * dpr), 0, physH) - y;
    if (w <= 0 || h <= 0) continue;
    wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)x, (uint32_t)y, (uint32_t)w,
                                        (uint32_t)h);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, uniformGroup, 0, nullptr);
    quads.draw(pass, cmd.quad0, cmd.quadN);
    text.draw(pass, cmd.glyph0, cmd.glyphN);
  }

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBufferDescriptor cmdDesc = {};
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmdDesc);
  wgpuCommandEncoderRelease(encoder);

  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);

  gpu_present(gpu);
  wgpuTextureViewRelease(view);
  wgpuTextureRelease(frameTex);
}
