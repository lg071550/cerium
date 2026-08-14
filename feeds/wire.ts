// SPSC ring writer over a SharedArrayBuffer + command-block reader.
// Single producer (this worker), single consumer (the WASM main thread).
//
// SAB layout:
//   offset  size  field
//   0       u32   magic (0x4345524d "CERM")
//   4       u32   writeIdx (monotonic; slot = writeIdx % capacity)
//   8       u32   readIdx  (written by consumer)
//   12      u32   capacity (event count)
//   16      u32   dropped  (incremented when the ring is full)
//   20      u32   cmdType  (consumer → worker command)
//   24      u32   cmdVenue
//   28      u32   pad
//   32      f64   cmdArg
//   40      f64   pad
//   48      u32   cmdSeq   (incremented per command)
//   52..64  pad
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
  SetCandles: 4, // arg = interval minutes
} as const;

const HEADER = 64;
const RECORD = 32;

export class WireWriter {
  private readonly u8: Uint8Array;
  private readonly u32: Uint32Array;
  private readonly f64: Float64Array;
  private readonly capacity: number;
  private writeIdx = 0;

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
    const w = this.writeIdx;
    const r = Atomics.load(this.u32, 2);
    if (w - r >= this.capacity) {
      Atomics.add(this.u32, 4, 1); // dropped — consumer will request resync
      return;
    }
    const base = HEADER + (w % this.capacity) * RECORD;
    this.f64[base >> 3] = price;
    this.f64[(base + 8) >> 3] = qty;
    this.f64[(base + 16) >> 3] = ts;
    this.u32[(base + 24) >> 2] = aux;
    this.u8[base + 28] = type;
    this.u8[base + 29] = venue;
    this.u8[base + 30] = side;
    this.u8[base + 31] = 0;
    this.writeIdx = w + 1;
    Atomics.store(this.u32, 1, this.writeIdx);
  }

  cmdSeq(): number {
    return Atomics.load(this.u32, 12);
  }

  command(): { type: number; venue: number; arg: number } {
    return {
      type: Atomics.load(this.u32, 5),
      venue: Atomics.load(this.u32, 6),
      arg: this.f64[4],
    };
  }
}
