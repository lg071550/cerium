#include "bridge.h"

#include <emscripten.h>

// Ring control block (bytes): see feeds/wire.ts for the full layout.
//   0 magic | 4 writeIdx | 8 readIdx | 12 capacity | 16 dropped
//  20 cmdType | 24 cmdVenue | 32 cmdArg(f64) | 48 cmdSeq

EM_JS(void, bridge_init_js, (int capacity), {
  try {
    if (!globalThis.crossOriginIsolated) {
      console.error("feeds: page is not crossOriginIsolated — SharedArrayBuffer unavailable");
      return;
    }
    var sab = new SharedArrayBuffer(64 + capacity * 32);
    var u8 = new Uint8Array(sab);
    var u32 = new Uint32Array(sab);
    Atomics.store(u32, 0, 0x4345524d);
    Atomics.store(u32, 3, capacity);
    Module._ceriumRing = { sab: sab, u8: u8, u32: u32, f64: new Float64Array(sab) };

    var worker = new Worker("build/feeds.worker.js", { type: "module" });
    worker.postMessage({ kind: "init", sab: sab, capacity: capacity });
    Module._ceriumWorker = worker;
  } catch (e) {
    console.error("feeds: bridge init failed", e);
  }
});

EM_JS(int, bridge_drain_js, (void* dest, int maxEvents), {
  var R = Module._ceriumRing;
  if (!R) return 0;
  var u32 = R.u32, u8 = R.u8;
  var w = Atomics.load(u32, 1);
  var r = Atomics.load(u32, 2);
  var avail = (w - r) >>> 0;
  var n = Math.min(avail, maxEvents);
  if (n === 0) return 0;
  var cap = u32[3];
  var out = dest;
  for (var i = 0; i < n; i++) {
    var slot = 64 + ((r + i) % cap) * 32;
    HEAPU8.set(u8.subarray(slot, slot + 32), out + i * 32);
  }
  Atomics.store(u32, 2, (r + n) >>> 0);
  return n;
});

EM_JS(int, bridge_take_dropped_js, (), {
  var R = Module._ceriumRing;
  if (!R) return 0;
  return Atomics.exchange(R.u32, 4, 0);
});

EM_JS(void, bridge_cmd_js, (unsigned type, unsigned venue, double arg), {
  var R = Module._ceriumRing;
  if (!R) return;
  Atomics.store(R.u32, 5, type);
  Atomics.store(R.u32, 6, venue);
  R.f64[4] = arg;
  Atomics.store(R.u32, 12, (Atomics.load(R.u32, 12) + 1) >>> 0);
});

namespace bridge {

void init() { bridge_init_js(65536); }

int drain(wire::Event* dest, int maxEvents) {
  return bridge_drain_js(dest, maxEvents);
}

int takeDropped() { return bridge_take_dropped_js(); }

void sendCommand(uint32_t type, uint32_t venue, double arg) {
  bridge_cmd_js(type, venue, arg);
}

} // namespace bridge
