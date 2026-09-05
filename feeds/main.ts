// Feeds worker entry: owns venue adapters, emits normalized binary events
// into the shared ring, and services consumer commands (resync, symbol, …).

import { CMD, EV, STATUS, WireWriter } from "./wire";
import type { L2Update, PriceLevel, TradePrint } from "./types";
import type { AdapterDeps, VenueAdapter } from "./venues/types";
import { SYMBOLS, VENUES, type VenueDef } from "./registry";
import { fetchCandles, fetchOrderFlow, TF_TIME } from "./candles";
import { startMarketFeed } from "./market";
import { configureHt } from "./hypertracker";

let writer: WireWriter | null = null;
let symbolIndex = 0;
let candleTf = { kind: TF_TIME, value: 1 }; // kind: 0=time(min) 1=tick 2=volume
let orderFlowRequested = false;
let candleRequest = 0;
let flowRequest = 0;
let orderFlowRetries = 0;
let candleRetries = 0;
const adapters: (VenueAdapter | null)[] = new Array(VENUES.length).fill(null);
const enabled = new Set<number>(VENUES.map((v) => v.index)); // all on by default

function postOrderFlow(
  forSymbol: number,
  tf: { kind: number; value: number },
  flow: Float64Array,
  prepend: boolean,
): void {
  (self as unknown as Worker).postMessage(
    {
      kind: "orderflow",
      sym: forSymbol,
      tfKind: tf.kind,
      tfValue: tf.value,
      flow,
      prepend: prepend ? 1 : 0,
    },
    [flow.buffer],
  );
}

function refreshOrderFlow(): void {
  const sym = SYMBOLS[symbolIndex];
  const tf = candleTf;
  const forSymbol = symbolIndex;
  const request = ++flowRequest;
  fetchOrderFlow(sym, tf.kind, tf.value, (flow, prepend) => {
    if (request !== flowRequest || forSymbol !== symbolIndex ||
        tf.kind !== candleTf.kind || tf.value !== candleTf.value) return false;
    if (flow.length === 0) return true;
    orderFlowRetries = 0;
    postOrderFlow(forSymbol, tf, flow, prepend);
    return true;
  }).then((n) => {
    if (request !== flowRequest || forSymbol !== symbolIndex) return;
    if (n === 0 && orderFlowRetries < 3) {
      const delay = 1500 * ++orderFlowRetries;
      setTimeout(() => {
        if (request === flowRequest && forSymbol === symbolIndex) refreshOrderFlow();
      }, delay);
    }
  });
}

function refreshCandles(): void {
  const sym = SYMBOLS[symbolIndex];
  const tf = candleTf;
  const forSymbol = symbolIndex;
  const request = ++candleRequest;
  // Progressive bootstrap for tick/volume TFs: post partial bar/flow
  // snapshots as the walk streams older pages, instead of leaving the chart
  // blank until the whole pull completes.
  const postPartial = (bars: Float64Array, flow: Float64Array): void => {
    if (request !== candleRequest || forSymbol !== symbolIndex ||
        tf.kind !== candleTf.kind || tf.value !== candleTf.value) return;
    (self as unknown as Worker).postMessage(
      {
        kind: "candles", sym: forSymbol, tfKind: tf.kind, tfValue: tf.value,
        data: bars, flow,
      },
      [bars.buffer, flow.buffer],
    );
  };
  // Supersede check for the network work itself, not just the result: without
  // it a symbol/TF switch mid-walk leaves the old walk paging to completion
  // (~6400 fapi weight per abandoned pull).
  const stale = (): boolean =>
    request !== candleRequest || forSymbol !== symbolIndex ||
    tf.kind !== candleTf.kind || tf.value !== candleTf.value;
  fetchCandles(sym, tf.kind, tf.value, orderFlowRequested && tf.kind !== TF_TIME, postPartial, stale).then((history) => {
    if (request !== candleRequest || forSymbol !== symbolIndex ||
        tf.kind !== candleTf.kind || tf.value !== candleTf.value) return;
    if (!history) {
      if (candleRetries >= 4) return;
      const delay = 700 * (1 << candleRetries++);
      setTimeout(() => {
        if (request === candleRequest && forSymbol === symbolIndex) refreshCandles();
      }, delay);
      return;
    }
    candleRetries = 0;
    const { bars, flow } = history;
    if (tf.kind !== TF_TIME && orderFlowRequested && flow.length === 0 &&
        orderFlowRetries < 3) {
      const delay = 1500 * ++orderFlowRetries;
      setTimeout(() => {
        if (request === candleRequest && forSymbol === symbolIndex) refreshCandles();
      }, delay);
    } else if (flow.length > 0) {
      orderFlowRetries = 0;
    }
    (self as unknown as Worker).postMessage(
      {
        kind: "candles", sym: forSymbol, tfKind: tf.kind, tfValue: tf.value,
        data: bars, flow,
      },
      [bars.buffer, flow.buffer],
    );
    // Klines first: kicking the aggTrade walk in parallel 429s the shared
    // www.binance.com WAF and is the usual reason a TF switch paints live-only.
    if (orderFlowRequested && tf.kind === TF_TIME) refreshOrderFlow();
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

function handleCommand(type: number, venue: number, arg: number): void {
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
    orderFlowRetries = 0;
    candleRetries = 0;
    ++flowRequest;
    for (const v of VENUES) stopVenue(v.index);
    for (const v of VENUES) if (enabled.has(v.index)) startVenue(v);
    refreshCandles();
    startMarketFeed(symbolIndex);
    return;
  }

  if (type === CMD.SetCandles) {
    // venue carries the timeframe kind (0=time 1=tick 2=volume), arg the value
    const kind = venue;
    const value = kind === TF_TIME ? Math.round(arg) : arg;
    if (kind < 0 || kind > 2 || !(value > 0) || value > 1e6) return;
    candleTf = { kind, value };
    orderFlowRetries = 0;
    candleRetries = 0;
    ++flowRequest;
    refreshCandles();
    return;
  }

  if (type === CMD.RequestOrderFlow) {
    orderFlowRequested = true;
    orderFlowRetries = 0;
    refreshOrderFlow();
    return;
  }
}

self.onmessage = (e: MessageEvent) => {
  const data = e.data;
  if (data?.kind === "init" && data.sab instanceof SharedArrayBuffer) {
    writer = new WireWriter(data.sab, data.capacity ?? 0);
    for (const v of VENUES) if (enabled.has(v.index)) startVenue(v);
    refreshCandles();
    startMarketFeed(symbolIndex);
    return;
  }
  if (data?.kind === "command" && writer) {
    const type = Number(data.type);
    const venue = Number(data.venue);
    const arg = Number(data.arg);
    if (Number.isSafeInteger(type) && Number.isSafeInteger(venue) && Number.isFinite(arg)) {
      handleCommand(type, venue, arg);
    }
    return;
  }
  if (data?.kind === "ht") {
    const token = typeof data.token === "string" ? data.token : "";
    const sym = Number(data.sym);
    configureHt({
      token,
      sym: Number.isSafeInteger(sym) ? sym : symbolIndex,
      liq: !!data.liq,
      sl: !!data.sl,
    });
  }
};
