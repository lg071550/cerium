import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { finiteNumber, isRecord, priceLevel, type AdapterDeps, type VenueAdapter } from "./types";

const PING_INTERVAL_MS = 20000;

export type ParsedBybit =
  | { kind: "snapshot"; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "delta"; updates: L2Update[] }
  | null;

export function parseBybitBook(msg: unknown): ParsedBybit {
  if (!isRecord(msg) || typeof msg.type !== "string" || !isRecord(msg.data)) return null;
  const b = msg.data.b;
  const a = msg.data.a;
  if (!Array.isArray(b) || !Array.isArray(a)) return null;

  if (msg.type === "snapshot") {
    return { kind: "snapshot", bids: toLevels(b), asks: toLevels(a) };
  }
  if (msg.type === "delta") {
    const updates: L2Update[] = [];
    for (const row of b) {
      const u = toUpdate(row, "bid");
      if (u) updates.push(u);
    }
    for (const row of a) {
      const u = toUpdate(row, "ask");
      if (u) updates.push(u);
    }
    return { kind: "delta", updates };
  }
  return null;
}

function toLevels(rows: unknown[]): PriceLevel[] {
  const out: PriceLevel[] = [];
  for (const row of rows) {
    const lvl = rowToLevel(row);
    if (lvl) out.push(lvl);
  }
  return out;
}

function toUpdate(row: unknown, side: L2Update["side"]): L2Update | null {
  const lvl = rowToLevel(row);
  return lvl ? { side, ...lvl } : null;
}

function rowToLevel(row: unknown): PriceLevel | null {
  if (!Array.isArray(row) || typeof row[0] !== "string" || typeof row[1] !== "string") return null;
  return priceLevel(row[0], row[1]);
}

export function parseBybitTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || typeof msg.topic !== "string" || !msg.topic.startsWith("publicTrade.")) {
    return null;
  }
  if (!Array.isArray(msg.data)) return null;
  const prints: TradePrint[] = [];
  for (const row of msg.data) {
    if (!isRecord(row)) return null;
    if (typeof row.p !== "string" || typeof row.v !== "string") return null;
    if (row.S !== "Buy" && row.S !== "Sell") return null;
    if (typeof row.T !== "number") return null;
    const level = priceLevel(row.p, row.v);
    const ts = finiteNumber(row.T);
    if (!level || level.size <= 0 || ts === null) return null;
    prints.push({
      ...level,
      side: row.S === "Buy" ? "buy" : "sell",
      ts,
    });
  }
  return prints;
}

export interface BybitAdapterOptions {
  id: string;
  symbol: string;
  wsUrl: string;
  topic: string;

  tradeTopic: string;
}

export class BybitBaseAdapter implements VenueAdapter {
  readonly id: string;
  readonly symbol: string;
  private readonly wsUrl: string;
  private readonly topic: string;
  private readonly tradeTopic: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private synced=false;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, opts: BybitAdapterOptions) {
    this.deps = deps;
    this.id = opts.id;
    this.symbol = opts.symbol;
    this.wsUrl = opts.wsUrl;
    this.topic = opts.topic;
    this.tradeTopic = opts.tradeTopic;
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
    this.synced=false;
    this.deps.setState("connecting");
    const ws = new WebSocket(this.wsUrl);
    this.ws = ws;

    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(JSON.stringify({ op: "subscribe", args: [this.topic] }));

      ws.send(JSON.stringify({ op: "subscribe", args: [this.tradeTopic] }));

      this.pingTimer = setInterval(() => {
        if (this.ws?.readyState === WebSocket.OPEN) {
          this.ws.send(JSON.stringify({ op: "ping" }));
        }
      }, PING_INTERVAL_MS);
    };

    ws.onmessage = (event) => this.handle(event);
    ws.onclose = () => this.conn.dropped("closed");
    ws.onerror = () => this.conn.dropped("socket error");
  }

  private teardown(): void {
    this.synced=false;
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
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    if (isRecord(raw) && (raw.op === "pong" || raw.ret_msg === "pong")) {
      this.deps.book.noteActivity();
      return;
    }
    const parsed = parseBybitBook(raw);
    if (!parsed) {

      const prints = parseBybitTrades(raw);
      if (prints) this.deps.onTrade?.(prints);
      return;
    }
    if (parsed.kind === "snapshot") {
      this.synced=true;
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.deps.setState("live");
    } else if(this.synced) {
      this.deps.book.applyUpdates(parsed.updates);
    }
  }
}

export class BybitAdapter extends BybitBaseAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "bybit",
      symbol: "ETHUSDT",
      wsUrl: "wss://stream.bybit.com/v5/public/spot",
      topic: "orderbook.1000.ETHUSDT",
      tradeTopic: "publicTrade.ETHUSDT",
    });
  }
}
