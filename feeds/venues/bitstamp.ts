import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://ws.bitstamp.net";
const DEFAULT_INST = "ethusd";

const MAX_SNAPSHOT_LEAD_US = 5_000_000;

export interface BitstampDiff {

  microtimestamp: number;
  updates: L2Update[];
}

export interface BitstampSnapshot {
  microtimestamp: number;
  bids: PriceLevel[];
  asks: PriceLevel[];
}

export function parseBitstamp(
  msg: unknown,
  channel = `diff_order_book_${DEFAULT_INST}`,
): BitstampDiff | null {
  if (!isRecord(msg) || msg.event !== "data" || msg.channel !== channel) return null;
  if (!isRecord(msg.data)) return null;
  const d = msg.data;
  if (typeof d.microtimestamp !== "string") return null;
  const microtimestamp = Number(d.microtimestamp);
  if (!Number.isFinite(microtimestamp)) return null;
  const bids = levels(d.bids);
  const asks = levels(d.asks);
  if (!bids || !asks) return null;
  const updates: L2Update[] = [];
  for (const l of bids) updates.push({ side: "bid", ...l });
  for (const l of asks) updates.push({ side: "ask", ...l });
  return { microtimestamp, updates };
}

export function parseBitstampSnapshot(raw: unknown): BitstampSnapshot | null {
  if (!isRecord(raw) || typeof raw.microtimestamp !== "string") return null;
  const microtimestamp = Number(raw.microtimestamp);
  if (!Number.isFinite(microtimestamp)) return null;
  const bids = levels(raw.bids);
  const asks = levels(raw.asks);
  if (!bids || !asks) return null;
  return { microtimestamp, bids, asks };
}

export function parseBitstampTrades(
  msg: unknown,
  channel = `live_trades_${DEFAULT_INST}`,
): TradePrint[] | null {
  if (!isRecord(msg) || msg.event !== "trade" || msg.channel !== channel) return null;
  if (!isRecord(msg.data)) return null;
  const d = msg.data;
  if (typeof d.price !== "number" || typeof d.amount !== "number") return null;
  if (typeof d.timestamp !== "string") return null;
  if (d.type !== 0 && d.type !== 1) return null;
  const ts = Number(d.timestamp) * 1000;
  if (!Number.isFinite(ts)) return null;
  return [{ price: d.price, size: d.amount, side: d.type === 0 ? "buy" : "sell", ts }];
}

function levels(v: unknown): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!Array.isArray(row)) return null;
    const p: unknown = row[0];
    const q: unknown = row[1];
    if (typeof p !== "string" || typeof q !== "string") return null;
    const price = Number(p);
    const size = Number(q);
    if (!Number.isFinite(price) || price <= 0 || !Number.isFinite(size) || size < 0) return null;
    out.push({ price, size });
  }
  return out;
}

export class BitstampAdapter implements VenueAdapter {
  readonly id = "bitstamp";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly restUrl: string;
  private readonly channel: string;
  private readonly tradesChannel: string;
  private ws: WebSocket | null = null;

  private buffer: BitstampDiff[] | null = null;
  private abortController: AbortController | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = DEFAULT_INST) {
    this.deps = deps;
    this.symbol = inst;
    this.restUrl = `https://www.bitstamp.net/api/v2/order_book/${inst}/?group=1`;
    this.channel = `diff_order_book_${inst}`;
    this.tradesChannel = `live_trades_${inst}`;
    this.conn = new Reconnect(
      deps.setState,
      () => this.open(),
      () => this.teardown(),
    );
  }

  start(): void {
    this.conn.start();
  }

  stop(): void {
    this.conn.stop();
    this.deps.book.clear();
  }

  private open(): void {
    this.deps.setState("connecting");
    this.buffer = [];
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      // Backoff resets only after the snapshot lands (see fetchSnapshot).
      this.deps.setState("syncing");
      ws.send(JSON.stringify({ event: "bts:subscribe", data: { channel: this.channel } }));

      ws.send(JSON.stringify({ event: "bts:subscribe", data: { channel: this.tradesChannel } }));

      void this.fetchSnapshot();
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.resync("closed");
    ws.onerror = () => this.resync("socket error");
  }

  private teardown(): void {

    this.abortController?.abort();
    this.abortController = null;
    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }
    this.deps.book.clear();
    this.buffer = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    if (isRecord(raw) && raw.event === "bts:heartbeat") {
      this.deps.book.noteActivity();
      return;
    }
    const diff = parseBitstamp(raw, this.channel);
    if (!diff) {

      const prints = parseBitstampTrades(raw, this.tradesChannel);
      if (prints) this.deps.onTrade?.(prints);
      return;
    }

    if (this.buffer) {

      if(this.buffer.length>=2000) {this.resync("snapshot buffer overflow"); return;}
      this.buffer.push(diff);
      return;
    }

    this.deps.book.applyUpdates(diff.updates);
  }

  private async fetchSnapshot(): Promise<void> {
    if (!this.conn.isRunning) return;
    this.abortController?.abort();
    const abort = new AbortController();
    this.abortController = abort;

    let snap: BitstampSnapshot;
    try {
      const res = await fetch(this.restUrl, { signal: AbortSignal.any([abort.signal, AbortSignal.timeout(15000)]) });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = parseBitstampSnapshot(await res.json());
      if (!data) throw new Error("malformed snapshot");
      snap = data;
    } catch {
      if (abort.signal.aborted || !this.conn.isRunning) return;
      this.resync("snapshot fetch failed");
      return;
    }

    if (abort.signal.aborted || !this.conn.isRunning) return;

    const remaining = (this.buffer ?? [])
      .filter((d) => d.microtimestamp > snap.microtimestamp)
      .sort((a, b) => a.microtimestamp - b.microtimestamp);
    const head = remaining[0];
    if (head && head.microtimestamp - snap.microtimestamp > MAX_SNAPSHOT_LEAD_US) {
      this.resync(
        `stale snapshot: first diff ${head.microtimestamp} leads snapshot ${snap.microtimestamp} by >${MAX_SNAPSHOT_LEAD_US / 1_000_000}s`,
      );
      return;
    }

    this.deps.book.applySnapshot(snap.bids, snap.asks);
    for (const d of remaining) this.deps.book.applyUpdates(d.updates);
    this.buffer = null;
    // Snapshot applied and diffs bridged — reset reconnect backoff here, not
    // on socket open.
    this.conn.connected();
    this.deps.setState("live");
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
