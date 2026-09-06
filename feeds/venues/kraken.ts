import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://ws.kraken.com/v2";
const DEPTH = 1000;
const PING_MS = 20_000;

export type ParsedKraken =
  | { kind: "snapshot"; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; updates: L2Update[] }
  | null;

export function parseKraken(msg: unknown): ParsedKraken {
  if (!isRecord(msg)) return null;
  if (msg.channel !== "book") return null;
  if (msg.type !== "snapshot" && msg.type !== "update") return null;
  if (!Array.isArray(msg.data)) return null;

  const bids: PriceLevel[] = [];
  const asks: PriceLevel[] = [];
  const updates: L2Update[] = [];
  for (const entry of msg.data) {
    if (!isRecord(entry)) return null;
    const b = levels(entry.bids);
    const a = levels(entry.asks);
    if (!b || !a) return null;
    if (msg.type === "snapshot") {
      bids.push(...b);
      asks.push(...a);
    } else {
      for (const l of b) updates.push({ side: "bid", ...l });
      for (const l of a) updates.push({ side: "ask", ...l });
    }
  }

  if (msg.type === "snapshot") return { kind: "snapshot", bids, asks };
  return { kind: "update", updates };
}

function levels(v: unknown): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!isRecord(row) || typeof row.price !== "number" || typeof row.qty !== "number") {
      return null;
    }
    out.push({ price: row.price, size: row.qty });
  }
  return out;
}

export function parseKrakenTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg)) return null;
  if (msg.channel !== "trade") return null;
  if (msg.type !== "update") return null;
  if (!Array.isArray(msg.data)) return null;

  const prints: TradePrint[] = [];
  for (const entry of msg.data) {
    if (!isRecord(entry)) return null;
    if (typeof entry.price !== "number" || typeof entry.qty !== "number") return null;
    if (entry.side !== "buy" && entry.side !== "sell") return null;
    const parsed = typeof entry.timestamp === "string" ? Date.parse(entry.timestamp) : NaN;
    prints.push({
      price: entry.price,
      size: entry.qty,
      side: entry.side,
      ts: Number.isNaN(parsed) ? Date.now() : parsed,
    });
  }
  return prints;
}

export class KrakenAdapter implements VenueAdapter {
  readonly id = "kraken";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private synced = false;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = "ETH/USD") {
    this.deps = deps;
    this.symbol = inst;
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
    this.synced = false;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({
          method: "subscribe",
          params: { channel: "book", symbol: [this.symbol], depth: DEPTH, snapshot: true },
        }),
      );

      ws.send(
        JSON.stringify({
          method: "subscribe",
          params: { channel: "trade", symbol: [this.symbol], snapshot: false },
        }),
      );
      this.pingTimer = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ method: "ping" }));
      }, PING_MS);
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.resync("closed");
    ws.onerror = () => this.resync("socket error");
  }

  private teardown(): void {
    if (this.pingTimer !== null) {
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
    this.synced = false;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    if (isRecord(raw) && (raw.method === "pong" || raw.channel === "heartbeat")) {
      this.deps.book.noteActivity();
      return;
    }
    const parsed = parseKraken(raw);
    if (!parsed) {

      const prints = parseKrakenTrades(raw);
      if (prints) this.deps.onTrade?.(prints);
      return;
    }

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.synced = true;
      this.deps.setState("live");
      return;
    }

    if (!this.synced) return;
    this.deps.book.applyUpdates(parsed.updates);
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
