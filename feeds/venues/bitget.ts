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

const WS_URL = "wss://ws.bitget.com/v2/ws/public";

const PING_INTERVAL_MS = 20000;

export type ParsedBitget =
  | { kind: "snapshot"; seq: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; seq: number; pseq: number; updates: L2Update[] }
  | null;

export function parseBitget(msg: unknown): ParsedBitget {
  if (!isRecord(msg) || typeof msg.action !== "string" || !Array.isArray(msg.data)) return null;
  const d: unknown = msg.data[0];
  if (!isRecord(d) || !Array.isArray(d.bids) || !Array.isArray(d.asks)) return null;
  const seq = safeInteger(d.seq);
  if (seq === null) return null;

  if (msg.action === "snapshot") {
    const bids = toLevels(d.bids);
    const asks = toLevels(d.asks);
    if (!bids || !asks) return null;
    return { kind: "snapshot", seq, bids, asks };
  }

  if (msg.action === "update") {
    const pseq = safeInteger(d.pseq);
    if (pseq === null) return null;
    const updates: L2Update[] = [];
    for (const row of d.bids) {
      const u = toUpdate(row, "bid");
      if (!u) return null;
      updates.push(u);
    }
    for (const row of d.asks) {
      const u = toUpdate(row, "ask");
      if (!u) return null;
      updates.push(u);
    }
    return { kind: "update", seq, pseq, updates };
  }

  return null;
}

function toLevels(rows: unknown[]): PriceLevel[] | null {
  const out: PriceLevel[] = [];
  for (const row of rows) {
    const lvl = rowToLevel(row);
    if (lvl === null) return null;
    if (lvl.size > 0) out.push(lvl);
  }
  return out;
}

function toUpdate(row: unknown, side: L2Update["side"]): L2Update | null {
  const lvl = rowToLevel(row);
  return lvl ? { side, ...lvl } : null;
}

function rowToLevel(row: unknown): PriceLevel | null {
  if (!Array.isArray(row)) return null;
  const px: unknown = row[0];
  const sz: unknown = row[1];
  if (typeof px !== "string" || typeof sz !== "string") return null;
  return priceLevel(px, sz);
}

export function parseBitgetTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || !isRecord(msg.arg)) return null;
  if (msg.arg.channel !== "trade") return null;
  if (!Array.isArray(msg.data)) return null;

  const prints: TradePrint[] = [];
  for (const row of msg.data) {
    if (!isRecord(row)) return null;
    if (
      typeof row.ts !== "string" ||
      typeof row.price !== "string" ||
      typeof row.size !== "string"
    ) {
      return null;
    }
    if (row.side !== "buy" && row.side !== "sell") return null;
    const level = priceLevel(row.price, row.size);
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

export interface BitgetAdapterOptions {
  id: FeedId;
  symbol: string;
  instType: "SPOT" | "USDT-FUTURES";
  // Wire instId; defaults to `symbol` (subclasses may keep a display label in
  // `symbol`, e.g. "ETHUSDT-PERP", while subscribing with "ETHUSDT").
  instId?: string;
}

export class BitgetBookAdapter implements VenueAdapter {
  readonly id: FeedId;
  readonly symbol: string;
  private readonly instType: "SPOT" | "USDT-FUTURES";
  private readonly instId: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private lastSeq: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, opts: BitgetAdapterOptions) {
    this.deps = deps;
    this.id = opts.id;
    this.symbol = opts.symbol;
    this.instType = opts.instType;
    this.instId = opts.instId ?? opts.symbol;
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
      ws.send(
        JSON.stringify({
          op: "subscribe",
          args: [{ instType: this.instType, channel: "books", instId: this.instId }],
        }),
      );

      ws.send(
        JSON.stringify({
          op: "subscribe",
          args: [{ instType: this.instType, channel: "trade", instId: this.instId }],
        }),
      );

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
    const parsed = parseBitget(raw);
    if (!parsed) {

      const prints = parseBitgetTrades(raw);
      if (prints) this.deps.onTrade?.(prints);
      return;
    }

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastSeq = parsed.seq;
      this.deps.setState("live");
      return;
    }

    if (this.lastSeq === null) return;

    if (parsed.pseq !== this.lastSeq) {
      this.resync(`seq gap: expected pseq ${this.lastSeq}, got ${parsed.pseq}`);
      return;
    }

    this.deps.book.applyUpdates(parsed.updates);
    this.lastSeq = parsed.seq;
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}

export class BitgetAdapter extends BitgetBookAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "bitget", symbol: "ETHUSDT", instType: "SPOT" });
  }
}
