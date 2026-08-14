#include "line_batch.h"
#include "pipeline.h"

#include <string>

// group(0) uniforms + srgb_to_linear come from wgsl_common() (pipeline.h)
static const char* kLineWGSL = R"wgsl(
struct VsIn {
  @builtin(vertex_index) vi: u32,
  @location(0) pts: vec4f,
  @location(1) color: vec4f,
  @location(2) params: vec4f,
};
struct VsOut {
  @builtin(position) pos: vec4f,
  @location(0) color: vec4f,
  @location(1) sideDist: f32, // signed distance from center line, physical px
  @location(2) halfThick: f32,
};

@vertex fn vs(in: VsIn) -> VsOut {
  var corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0),
    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0));
  let c = corners[in.vi];

  let p0 = in.pts.xy * u.scale;
  let p1 = in.pts.zw * u.scale;
  let dp = p1 - p0;
  let len = length(dp);
  var dir = vec2f(1.0, 0.0);
  if (len > 0.001) { dir = dp / len; }
  let halfT = in.params.x * u.scale * 0.5;
  let n = vec2f(-dir.y, dir.x) * halfT;

  let base = mix(p0, p1, c.x);
  let side = c.y * 2.0 - 1.0;
  let px = base + n * side;
  let clip = vec2f(px.x / u.screen.x * 2.0 - 1.0, 1.0 - px.y / u.screen.y * 2.0);

  var out: VsOut;
  out.pos = vec4f(clip, 0.0, 1.0);
  out.color = in.color;
  out.sideDist = side * halfT;
  out.halfThick = halfT;
  return out;
}

@fragment fn fs(in: VsOut) -> @location(0) vec4f {
  let a = clamp(in.halfThick - abs(in.sideDist) + 0.5, 0.0, 1.0);
  let rgb = vec3f(srgb_to_linear(in.color.x), srgb_to_linear(in.color.y),
                  srgb_to_linear(in.color.z));
  return vec4f(rgb, in.color.a * a);
}
)wgsl";

void LineBatch::init(WGPUDevice dev, WGPUTextureFormat format,
                     WGPUBindGroupLayout uniformLayout) {
  device = dev;
  std::string src = std::string(wgsl_common()) + kLineWGSL;
  pipeline = pipeline_instanced(dev, format, src.c_str(), "line", sizeof(LineInstance),
                                &uniformLayout, 1);
}

void LineBatch::upload(WGPUQueue queue, const LineInstance* data, uint32_t count) {
  if (count == 0) return;
  if (count > capacity) {
    if (instances) wgpuBufferRelease(instances);
    capacity = count + count / 2;
    WGPUBufferDescriptor desc = {};
    desc.size = sizeof(LineInstance) * capacity;
    desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    instances = wgpuDeviceCreateBuffer(device, &desc);
  }
  wgpuQueueWriteBuffer(queue, instances, 0, data, sizeof(LineInstance) * count);
}

void LineBatch::draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count,
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
