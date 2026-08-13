// Feeds worker entry: owns venue adapters, emits normalized binary events
// into the shared ring, and services consumer commands (resync, symbol, …).

import { CMD, EV, STATUS, WireWriter } from "./wire";
import type { L2Update, PriceLevel, TradePrint } from "./types";
import type { AdapterDeps, VenueAdapter } from "./venues/types";
import { BinancePerpAdapter } from "./venues/binancePerp";

// Venue registry — index must match the C++ side (src/data/feeds.cpp).
const VENUES: { id: number; make: (deps: AdapterDeps) => VenueAdapter }[] = [
  { id: 0, make: (deps) => new BinancePerpAdapter(deps) },
];

let writer: WireWriter | null = null;
const adapters: (VenueAdapter | null)[] = [];

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

let lastCmdSeq = 0;

function pollCommands(): void {
  if (!writer) return;
  const seq = writer.cmdSeq();
  if (seq === lastCmdSeq) return;
  lastCmdSeq = seq;
  const { type, venue } = writer.command();
  if (type === CMD.ResyncVenue) {
    const a = adapters[venue];
    if (a) {
      a.stop();
      a.start();
    }
  }
  // SetSymbol / SetVenueEnabled land with later phases (D2/D3)
}

self.onmessage = (e: MessageEvent) => {
  const data = e.data;
  if (data?.kind === "init" && data.sab instanceof SharedArrayBuffer) {
    writer = new WireWriter(data.sab, data.capacity ?? 0);
    for (const v of VENUES) {
      const a = v.make(makeDeps(v.id));
      adapters[v.id] = a;
      a.start();
    }
    setInterval(pollCommands, 100);
  }
};
