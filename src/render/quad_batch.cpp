#include "quad_batch.h"
#include "pipeline.h"

#include <string>

// group(0) uniforms + srgb_to_linear come from wgsl_common() (pipeline.h)
static const char* kQuadWGSL = R"wgsl(
struct VsIn {
  @builtin(vertex_index) vi: u32,
  @location(0) rect: vec4f,
  @location(1) color: vec4f,
  @location(2) params: vec4f, // radius, border, softness (shadow), inflate
};
struct VsOut {
  @builtin(position) pos: vec4f,
  @location(0) color: vec4f,
  @location(1) local: vec2f, // physical px within rect
  @location(2) size: vec2f,  // physical px
  @location(3) radius: f32,  // physical px
  @location(4) border: f32,  // physical px; >0 → hollow outline band
  @location(5) soft: f32,    // physical px; >0 → soft shadow falloff
};

@vertex fn vs(in: VsIn) -> VsOut {
  var corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0),
    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0));
  let c = corners[in.vi];

  // geometry inflates by params.w (logical px) so soft shadows have room;
  // the SDF shape stays the original rect
  let inflate = in.params.w;
  let geomXY = in.rect.xy - vec2f(inflate);
  let geomWH = in.rect.zw + vec2f(inflate * 2.0);
  let px = (geomXY + c * geomWH) * u.scale;
  let clip = vec2f(px.x / u.screen.x * 2.0 - 1.0, 1.0 - px.y / u.screen.y * 2.0);

  var out: VsOut;
  out.pos = vec4f(clip, 0.0, 1.0);
  out.color = in.color;
  out.local = c * geomWH * u.scale - vec2f(inflate * u.scale);
  out.size = in.rect.zw * u.scale;
  out.radius = in.params.x * u.scale;
  out.border = in.params.y * u.scale;
  out.soft = in.params.z * u.scale;
  return out;
}

@fragment fn fs(in: VsOut) -> @location(0) vec4f {
  let r = min(in.radius, min(in.size.x, in.size.y) * 0.5);
  let b = in.size * 0.5 - vec2f(r);
  let q = abs(in.local - in.size * 0.5) - b;
  let d = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - r;

  var a: f32;
  if (in.soft > 0.5) {
    // shadow mode: full coverage inside the shape, smooth falloff decaying
    // to zero `soft` px OUTSIDE the edge (d > 0 is outside)
    let t = clamp(1.0 - d / in.soft, 0.0, 1.0);
    a = t * t * (3.0 - 2.0 * t);
  } else {
    a = clamp(0.5 - d, 0.0, 1.0); // 1px physical AA band
    if (in.border > 0.0) {
      // hollow: keep only the outer `border`-thick band
      a -= clamp(0.5 - (d + in.border), 0.0, 1.0);
    }
  }
  let rgb = vec3f(srgb_to_linear(in.color.x), srgb_to_linear(in.color.y),
                  srgb_to_linear(in.color.z));
  return vec4f(rgb, in.color.a * a);
}
)wgsl";

void QuadBatch::init(WGPUDevice dev, WGPUTextureFormat format,
                     WGPUBindGroupLayout uniformLayout) {
  device = dev;
  std::string src = std::string(wgsl_common()) + kQuadWGSL;
  pipeline = pipeline_instanced(dev, format, src.c_str(), "quad", sizeof(QuadInstance),
                                &uniformLayout, 1);
}

void QuadBatch::upload(WGPUQueue queue, const QuadInstance* data, uint32_t count) {
  if (count == 0) return;
  if (count > capacity) {
    if (instances) wgpuBufferRelease(instances);
    capacity = count + count / 2;
    WGPUBufferDescriptor desc = {};
    desc.size = sizeof(QuadInstance) * capacity;
    desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    instances = wgpuDeviceCreateBuffer(device, &desc);
  }
  wgpuQueueWriteBuffer(queue, instances, 0, data, sizeof(QuadInstance) * count);
}

void QuadBatch::draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count,
                     WGPURenderPipeline& lastPipe, WGPUBuffer& lastVB) const {
  if (count == 0) return;
  if (pipeline != lastPipe) {
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    lastPipe = pipeline;
  }
  if (instances != lastVB) {
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, instances, 0, WGPU_WHOLE_SIZE);
    lastVB = instances;
  }
  wgpuRenderPassEncoderDraw(pass, 6, count, 0, first);
}
