import type { FeedId, L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import {
  finiteNumber,
  isRecord,
  priceLevel,
  safeInteger,
  type AdapterDeps,
  type VenueAdapter,
} from "./types";

const WS_URL = "wss://ws.okx.com:8443/ws/v5/public";

const PING_INTERVAL_MS = 25000;

export type ParsedOkx =
  | { kind: "snapshot"; seqId: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; seqId: number; prevSeqId: number; updates: L2Update[] }
  | null;

export function parseOkxBook(msg: unknown, ctVal: number): ParsedOkx {
  if (!isRecord(msg) || typeof msg.action !== "string" || !Array.isArray(msg.data)) return null;
  const d: unknown = msg.data[0];
  if (!isRecord(d) || !Array.isArray(d.bids) || !Array.isArray(d.asks)) return null;
  const seqId = safeInteger(d.seqId);
  if (seqId === null) return null;

  if (msg.action === "snapshot") {
    const bids = toLevels(d.bids, ctVal);
    const asks = toLevels(d.asks, ctVal);
    if (!bids || !asks) return null;
    return { kind: "snapshot", seqId, bids, asks };
  }

  if (msg.action === "update") {
    const prevSeqId = safeInteger(d.prevSeqId);
    if (prevSeqId === null) return null;
    const updates: L2Update[] = [];
    for (const row of d.bids) {
      const u = toUpdate(row, "bid", ctVal);
      if (!u) return null;
      updates.push(u);
    }
    for (const row of d.asks) {
      const u = toUpdate(row, "ask", ctVal);
      if (!u) return null;
      updates.push(u);
    }
    return { kind: "update", seqId, prevSeqId, updates };
  }

  return null;
}

export function parseOkxTrades(msg: unknown, ctVal: number): TradePrint[] | null {
  if (!isRecord(msg) || !isRecord(msg.arg) || msg.arg.channel !== "trades") return null;
  if (!Array.isArray(msg.data)) return null;
  const prints: TradePrint[] = [];
  for (const row of msg.data) {
    if (!isRecord(row)) return null;
    if (typeof row.px !== "string" || typeof row.sz !== "string") return null;
    if (row.side !== "buy" && row.side !== "sell") return null;
    if (typeof row.ts !== "string") return null;
    const level = priceLevel(row.px, row.sz, ctVal);
    const ts = finiteNumber(row.ts);
    if (!level || level.size <= 0 || ts === null) return null;
    prints.push({
      ...level,
      side: row.side,
      ts,
    });
  }
  return prints;
}

function toLevels(rows: unknown[], ctVal: number): PriceLevel[] | null {
  const out: PriceLevel[] = [];
  for (const row of rows) {
    const lvl = rowToLevel(row, ctVal);
    if (lvl === null) return null;
    if (lvl.size > 0) out.push(lvl);
  }
  return out;
}

function toUpdate(row: unknown, side: L2Update["side"], ctVal: number): L2Update | null {
  const lvl = rowToLevel(row, ctVal);
  return lvl ? { side, ...lvl } : null;
}

function rowToLevel(row: unknown, ctVal: number): PriceLevel | null {
  if (!Array.isArray(row)) return null;
  const px: unknown = row[0];
  const sz: unknown = row[1];
  if (typeof px !== "string" || typeof sz !== "string") return null;
  return priceLevel(px, sz, ctVal);
}

export interface OkxAdapterOptions {
  id: FeedId;
  instId: string;

  ctVal: number;
}

export class OkxBookAdapter implements VenueAdapter {
  readonly id: FeedId;
  readonly symbol: string;
  private readonly instId: string;
  private readonly ctVal: number;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private lastSeq: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, opts: OkxAdapterOptions) {
    this.deps = deps;
    this.id = opts.id;
    this.symbol = opts.instId;
    this.instId = opts.instId;
    this.ctVal = opts.ctVal;
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
    this.lastSeq = null;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;

    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(JSON.stringify({ op: "subscribe", args: [{ channel: "books", instId: this.instId }] }));

      ws.send(JSON.stringify({ op: "subscribe", args: [{ channel: "trades", instId: this.instId }] }));

      this.pingTimer = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) ws.send("ping");
      }, PING_INTERVAL_MS);
    };

    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.resync("closed");
    ws.onerror = () => this.resync("socket error");
  }

  private teardown(): void {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }
    this.deps.book.clear();
    this.lastSeq = null;
  }

  private handle(event: MessageEvent): void {

    if (event.data === "pong") {
      this.deps.book.noteActivity();
      return;
    }
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    const parsed = parseOkxBook(raw, this.ctVal);
    if (!parsed) {

      const prints = parseOkxTrades(raw, this.ctVal);
      if (prints) this.deps.onTrade?.(prints);
      return;
    }

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastSeq = parsed.seqId;
      this.deps.setState("live");
      return;
    }

    if (this.lastSeq !== null && parsed.prevSeqId !== this.lastSeq) {
      this.resync(`seq gap: expected prev ${this.lastSeq}, got ${parsed.prevSeqId}`);
      return;
    }

    this.deps.book.applyUpdates(parsed.updates);
    this.lastSeq = parsed.seqId;
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}

export class OkxAdapter extends OkxBookAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "okx", instId: "ETH-USDT", ctVal: 1 });
  }
}
