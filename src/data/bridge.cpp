#include "bridge.h"

#include <emscripten.h>

// Ring control block (bytes): see feeds/wire.ts for the full layout.
//   0 magic | 4 writeIdx | 8 readIdx | 12 capacity | 16 dropped | 20..63 reserved

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
    // malloc of a large orderflow window can grow wasm memory. Emscripten's
    // HEAPF64 view is not always rebuilt (resizable buffers skip
    // updateMemoryViews), so HEAPF64.set would RangeError / write into a
    // stale buffer and cerium_on_orderflow would see zeros — CLUSTER then
    // keeps the kline gutter with no tick cells. Rebuild the view from the
    // live wasm buffer after every malloc.
        var copyF64 = function (src) {
      var n = src.length;
      var ptr = _malloc(Math.max(n, 1) * 8);
      if (!ptr) {
        console.error("feeds: malloc failed", n);
        return 0;
      }
      if (!n) return ptr;
      var mem = (Module.wasmMemory || wasmMemory);
      HEAPF64 = new Float64Array(mem.buffer);
      Module.HEAPF64 = HEAPF64;
      var idx = ptr >> 3;
      if (idx + n > HEAPF64.length) {
        console.error("feeds: heap copy overflow", ptr, n, HEAPF64.length);
        _free(ptr);
        return 0;
      }
      HEAPF64.set(src, idx);
      return ptr;
    };
    worker.onmessage = function (e) {
      var d = e.data;
      if (!d) return;
      if (d.kind === "diag") {
        // worker-side walk telemetry, surfaced on the page console for probes
        console.log(d.text);
        return;
      }
      if (d.kind === "candles") {
        try {
          var arr = d.data; // Float64Array: [ts, o, h, l, c, vol, takerBuyVol] × n
          var ptr = copyF64(arr);
          if (!ptr) return;
          _cerium_on_candles(ptr, arr.length / 7, d.tfKind, d.tfValue, d.sym);
          _free(ptr);
          var flow = d.flow; // [ts, price, qty, side] × n
          if (flow && flow.length) {
            var flowPtr = copyF64(flow);
            if (!flowPtr) return;
            _cerium_on_orderflow(flowPtr, flow.length / 4, d.tfKind, d.tfValue, d.sym, 0);
            _free(flowPtr);
          }
        } catch (err) {
          console.error("feeds: candles apply failed", err);
        }
        return;
      }
      if (d.kind === "orderflow") {
        try {
          var of = d.flow;
          if (of && of.length) {
            var ofPtr = copyF64(of);
            if (!ofPtr) return;
            _cerium_on_orderflow(ofPtr, of.length / 4, d.tfKind, d.tfValue, d.sym,
                                d.prepend ? 1 : 0);
            _free(ofPtr);
          }
        } catch (err) {
          console.error("feeds: orderflow apply failed", err);
        }
        return;
      }
      if (d.kind === "market") {
        try {
          var oi = d.oi, funding = d.funding, liq = d.liq;
          var oiPtr = copyF64(oi);
          var fPtr = copyF64(funding);
          var lPtr = copyF64(liq);
          if (!oiPtr || !fPtr || !lPtr) {
            if (oiPtr) _free(oiPtr);
            if (fPtr) _free(fPtr);
            if (lPtr) _free(lPtr);
            return;
          }
          _cerium_on_market(oiPtr, oi.length / 2, fPtr, funding.length / 2,
                            lPtr, liq.length / 4, d.sym, d.liqBase || 0);
          _free(oiPtr); _free(fPtr); _free(lPtr);
        } catch (err) {
          console.error("feeds: market apply failed", err);
        }
        return;
      }
      if (d.kind === "marketLiq") {
        // incremental liquidation print: [ts, price, qty, side] × n at global
        // record index d.start — appended without re-copying rolling history
        try {
          var liqRows = d.rows;
          var liqPtr = copyF64(liqRows);
          if (!liqPtr) return;
          _cerium_on_liq(liqPtr, liqRows.length / 4, d.start, d.sym);
          _free(liqPtr);
        } catch (err) {
          console.error("feeds: liq apply failed", err);
        }
      }
    };
    Module._ceriumWorker = worker;
  } catch (e) {
    console.error("feeds: bridge init failed", e);
  }
});

EM_JS(int, bridge_drain_js, (void* dest, int maxEvents), {
  var R = Module._ceriumRing;
  if (!R) return 0;
  // The heap views can go stale after a growth malloc (see copyF64 above);
  // the ring SAB never grows, so only the wasm-side view needs revalidating.
  var mem = (Module.wasmMemory || wasmMemory);
  if (HEAPU8.buffer !== mem.buffer) {
    HEAPU8 = new Uint8Array(mem.buffer);
    Module.HEAPU8 = HEAPU8;
  }
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
  var worker = Module._ceriumWorker;
  if (!worker) return;
  // Worker messages are ordered and queued. The previous single SAB mailbox
  // could overwrite commands before its 100 ms poll (notably all but the last
  // venue resync after a ring overflow).
  worker.postMessage({ kind: "command", type: type, venue: venue, arg: arg });
});

namespace bridge {

// The consumer drains until empty every rAF and overflow already triggers a
// full venue resync. 128 Ki events leaves eight complete drain batches (and
// roughly 10+ seconds at the measured live peak) without permanently charging
// every tab for the old 32 MiB, million-event backlog.
static constexpr int kRingCapacity = 1 << 17;

void init() { bridge_init_js(kRingCapacity); } // 128 Ki events = 4 MiB ring

int drain(wire::Event* dest, int maxEvents) {
  return bridge_drain_js(dest, maxEvents);
}

int takeDropped() { return bridge_take_dropped_js(); }

void sendCommand(uint32_t type, uint32_t venue, double arg) {
  bridge_cmd_js(type, venue, arg);
}

} // namespace bridge
