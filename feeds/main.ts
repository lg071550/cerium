// Feeds worker entry: owns venue adapters, emits normalized binary events
// into the shared ring, and services consumer commands (resync, symbol, …).

import { CMD, EV, STATUS, WireWriter } from "./wire";
import type { L2Update, PriceLevel, TradePrint } from "./types";
import type { AdapterDeps, VenueAdapter } from "./venues/types";
import { SYMBOLS, VENUES, type VenueDef } from "./registry";
import { fetchKlines } from "./candles";

let writer: WireWriter | null = null;
let symbolIndex = 0;
let candleInterval = 1;
const adapters: (VenueAdapter | null)[] = new Array(VENUES.length).fill(null);
const enabled = new Set<number>(VENUES.map((v) => v.index)); // all on by default

function refreshCandles(): void {
  const sym = SYMBOLS[symbolIndex];
  const interval = candleInterval;
  const forSymbol = symbolIndex;
  fetchKlines(sym, interval).then((data) => {
    if (!data) return;
    (self as unknown as Worker).postMessage(
      { kind: "candles", sym: forSymbol, interval, data },
      [data.buffer],
    );
  });
}

function makeDeps(venue: number): AdapterDeps {
  const w = writer!;
  return {
    book: {
      clear() {
        w.push(EV.SnapshotBegin, venue, 0, 0, 0, Date.now());
        w.push(EV.SnapshotEnd, venue, 0, 0, 0, Date.now());
      },
      applySnapshot(bids: readonly PriceLevel[], asks: readonly PriceLevel[]) {
        w.push(EV.SnapshotBegin, venue, 0, 0, 0, Date.now());
        for (const l of bids) w.push(EV.SnapshotLevel, venue, 0, l.price, l.size, Date.now());
        for (const l of asks) w.push(EV.SnapshotLevel, venue, 1, l.price, l.size, Date.now());
        w.push(EV.SnapshotEnd, venue, 0, 0, 0, Date.now());
      },
      applyUpdates(updates: readonly L2Update[]) {
        for (const u of updates)
          w.push(EV.BookUpdate, venue, u.side === "bid" ? 0 : 1, u.price, u.size, Date.now());
      },
      noteActivity() {},
    },
    setState(state) {
      w.push(EV.FeedStatus, venue, 0, 0, 0, Date.now(), STATUS[state] ?? 0);
    },
    onTrade(prints: TradePrint[]) {
      for (const p of prints)
        w.push(EV.Trade, venue, p.side === "buy" ? 0 : 1, p.price, p.size, p.ts);
    },
  };
}

function startVenue(v: VenueDef): void {
  const a = v.make(makeDeps(v.index), SYMBOLS[symbolIndex]);
  adapters[v.index] = a;
  a?.start();
}

function stopVenue(index: number): void {
  const a = adapters[index];
  adapters[index] = null;
  a?.stop();
}

let lastCmdSeq = 0;

function pollCommands(): void {
  if (!writer) return;
  const seq = writer.cmdSeq();
  if (seq === lastCmdSeq) return;
  lastCmdSeq = seq;
  const { type, venue, arg } = writer.command();

  if (type === CMD.ResyncVenue) {
    const a = adapters[venue];
    if (a) {
      a.stop();
      a.start();
    }
    return;
  }

  if (type === CMD.SetVenueEnabled) {
    const def = VENUES[venue];
    if (!def) return;
    if (arg === 0) {
      if (!enabled.has(venue)) return;
      enabled.delete(venue);
      stopVenue(venue);
    } else {
      if (enabled.has(venue)) return;
      enabled.add(venue);
      startVenue(def); // make() returns null when unsupported → stays null
    }
    return;
  }

  if (type === CMD.SetSymbol) {
    const next = Math.floor(arg);
    if (next === symbolIndex || next < 0 || next >= SYMBOLS.length) return;
    symbolIndex = next;
    for (const v of VENUES) stopVenue(v.index);
    for (const v of VENUES) if (enabled.has(v.index)) startVenue(v);
    refreshCandles();
    return;
  }

  if (type === CMD.SetCandles) {
    const minutes = Math.floor(arg);
    if (minutes <= 0 || minutes === candleInterval) return;
    candleInterval = minutes;
    refreshCandles();
  }
}

self.onmessage = (e: MessageEvent) => {
  const data = e.data;
  if (data?.kind === "init" && data.sab instanceof SharedArrayBuffer) {
    writer = new WireWriter(data.sab, data.capacity ?? 0);
    for (const v of VENUES) if (enabled.has(v.index)) startVenue(v);
    refreshCandles();
    setInterval(pollCommands, 100);
  }
};
