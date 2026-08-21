// SPSC ring writer over a SharedArrayBuffer. Consumer commands travel as
// ordered worker messages; bytes 20..63 stay reserved for wire compatibility.
// Single producer (this worker), single consumer (the WASM main thread).
//
// SAB layout:
//   offset  size  field
//   0       u32   magic (0x4345524d "CERM")
//   4       u32   writeIdx (monotonic; slot = writeIdx % capacity)
//   8       u32   readIdx  (written by consumer)
//   12      u32   capacity (event count)
//   16      u32   dropped  (incremented when the ring is full)
//   20..64        reserved
//   64      ...   capacity × 32-byte event records
//
// Event record (32 bytes, 8-aligned fields first):
//   +0  f64 price
//   +8  f64 qty
//   +16 f64 ts
//   +24 u32 aux
//   +28 u8  type
//   +29 u8  venue
//   +30 u8  side
//   +31 u8  flags

export const EV = {
  SnapshotBegin: 1,
  SnapshotLevel: 2,
  SnapshotEnd: 3,
  BookUpdate: 4,
  Trade: 5,
  FeedStatus: 6,
} as const;

export const STATUS: Record<string, number> = {
  connecting: 1,
  syncing: 2,
  live: 3,
  reconnecting: 4,
  error: 5,
};

export const CMD = {
  None: 0,
  ResyncVenue: 1,
  SetSymbol: 2,
  SetVenueEnabled: 3,
  SetCandles: 4, // venue = timeframe kind (0=time 1=tick 2=volume), arg = value
  RequestOrderFlow: 5, // lazy exact aggTrade history for footprint chart modes
} as const;

const HEADER = 64;
const RECORD = 32;

interface PendingEvent {
  type: number;
  venue: number;
  side: number;
  price: number;
  qty: number;
  ts: number;
  aux: number;
}

export class WireWriter {
  private readonly u8: Uint8Array;
  private readonly u32: Uint32Array;
  private readonly f64: Float64Array;
  private readonly capacity: number;
  private writeIdx = 0;
  private readonly pending: PendingEvent[] = [];
  private pendingHead = 0;
  private flushTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(sab: SharedArrayBuffer, expectedCapacity: number) {
    this.u8 = new Uint8Array(sab);
    this.u32 = new Uint32Array(sab);
    this.f64 = new Float64Array(sab);
    this.capacity = Math.floor((sab.byteLength - HEADER) / RECORD);
    if (this.capacity !== expectedCapacity) {
      console.warn("wire: capacity mismatch", this.capacity, expectedCapacity);
    }
    Atomics.store(this.u32, 0, 0x4345524d);
    Atomics.store(this.u32, 3, this.capacity);
  }

  push(type: number, venue: number, side: number, price: number, qty: number,
       ts: number, aux = 0): void {
    this.pending.push({ type, venue, side, price, qty, ts, aux });
    this.flushPending();
  }

  private flushPending(): void {
    while (this.pendingHead < this.pending.length) {
      if (!this.tryWrite(this.pending[this.pendingHead]!)) break;
      this.pendingHead++;
    }
    if (this.pendingHead > 0) {
      if (this.pendingHead === this.pending.length) {
        this.pending.length = 0;
        this.pendingHead = 0;
      } else if (this.pendingHead > 2048) {
        this.pending.splice(0, this.pendingHead);
        this.pendingHead = 0;
      }
    }
    const queued = this.pending.length - this.pendingHead;
    if (queued === 0) {
      if (this.flushTimer !== null) {
        clearTimeout(this.flushTimer);
        this.flushTimer = null;
      }
      return;
    }
    // Full-depth snapshots (Coinbase / Bitstamp / Binance spot 5k) are tens of
    // thousands of SnapshotLevel events. Dropping them used to truncate the
    // book and trigger a 27-venue resync, which is why the heatmap/ladder
    // only showed a thin near-mid ribbon. Queue until the consumer drains.
    if (queued > this.capacity * 4) {
      this.pending.length = 0;
      this.pendingHead = 0;
      Atomics.add(this.u32, 4, 1);
      if (this.flushTimer !== null) {
        clearTimeout(this.flushTimer);
        this.flushTimer = null;
      }
      return;
    }
    if (this.flushTimer === null) {
      this.flushTimer = setTimeout(() => {
        this.flushTimer = null;
        this.flushPending();
      }, 4);
    }
  }

  private tryWrite(ev: PendingEvent): boolean {
    const w = this.writeIdx >>> 0;
    const r = Atomics.load(this.u32, 2);
    // Both indices are u32 monotonic counters. Keep the distance unsigned so
    // wraparound after 2^32 events does not make the ring appear full forever.
    if (((w - r) >>> 0) >= this.capacity) return false;
    const base = HEADER + (w % this.capacity) * RECORD;
    this.f64[base >> 3] = ev.price;
    this.f64[(base + 8) >> 3] = ev.qty;
    this.f64[(base + 16) >> 3] = ev.ts;
    this.u32[(base + 24) >> 2] = ev.aux;
    this.u8[base + 28] = ev.type;
    this.u8[base + 29] = ev.venue;
    this.u8[base + 30] = ev.side;
    this.u8[base + 31] = 0;
    this.writeIdx = (w + 1) >>> 0;
    Atomics.store(this.u32, 1, this.writeIdx);
    return true;
  }
}
