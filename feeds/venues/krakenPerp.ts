import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, safeInteger, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://futures.kraken.com/ws/v1";
const PING_MS = 50_000;

export type ParsedKrakenPerp =
  | { kind: "snapshot"; seq: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; seq: number; update: L2Update }
  | null;

export function parseKrakenPerp(msg: unknown, inverse = true): ParsedKrakenPerp {
  if (!isRecord(msg)) return null;

  if (msg.feed === "book_snapshot") {
    const seq = safeInteger(msg.seq);
    if (seq === null) return null;
    const bids = levels(msg.bids, inverse);
    const asks = levels(msg.asks, inverse);
    if (!bids || !asks) return null;
    return { kind: "snapshot", seq, bids, asks };
  }

  if (msg.feed === "book") {
    const seq = safeInteger(msg.seq);
    const price = typeof msg.price === "number" ? msg.price : null;
    const qty = typeof msg.qty === "number" ? msg.qty : null;
    if (
      seq === null ||
      price === null ||
      qty === null ||
      !Number.isFinite(price) ||
      !Number.isFinite(qty) ||
      price <= 0 ||
      qty < 0
    ) return null;
    if (msg.side !== "buy" && msg.side !== "sell") return null;
    const side = msg.side === "buy" ? "bid" : "ask";

    return { kind: "update", seq, update: { side, price, size: inverse ? qty / price : qty } };
  }

  return null;
}

function levels(v: unknown, inverse: boolean): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!isRecord(row) || typeof row.price !== "number" || typeof row.qty !== "number") {
      return null;
    }
    if (!Number.isFinite(row.price) || !Number.isFinite(row.qty) || row.price <= 0 || row.qty < 0) {
      return null;
    }
    out.push({ price: row.price, size: inverse ? row.qty / row.price : row.qty });
  }
  return out;
}

export function parseKrakenPerpTrades(msg: unknown, inverse = true): TradePrint[] | null {
  if (!isRecord(msg)) return null;
  if (msg.feed !== "trade") return null;
  const price = typeof msg.price === "number" ? msg.price : null;
  const qty = typeof msg.qty === "number" ? msg.qty : null;
  const time = typeof msg.time === "number" ? msg.time : null;
  if (
    price === null ||
    qty === null ||
    time === null ||
    !Number.isFinite(price) ||
    !Number.isFinite(qty) ||
    !Number.isFinite(time) ||
    price <= 0 ||
    qty <= 0
  ) return null;
  if (msg.side !== "buy" && msg.side !== "sell") return null;
  return [{ price, size: inverse ? qty / price : qty, side: msg.side, ts: time }];
}

export class KrakenPerpAdapter implements VenueAdapter {
  readonly id = "kraken-perp";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private lastSeq: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = "PI_ETHUSD") {
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
    this.lastSeq = null;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({ event: "subscribe", feed: "book", product_ids: [this.symbol] }),
      );

      ws.send(
        JSON.stringify({ event: "subscribe", feed: "trade", product_ids: [this.symbol] }),
      );

      ws.send(JSON.stringify({ event: "subscribe", feed: "heartbeat" }));
      this.pingTimer = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ event: "ping" }));
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
    this.lastSeq = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    const parsed = parseKrakenPerp(raw, this.symbol.startsWith("PI_"));
    if (!parsed) {
      if (isRecord(raw) && raw.feed === "heartbeat") {

        this.deps.book.noteActivity();
      } else {

        const prints = parseKrakenPerpTrades(raw, this.symbol.startsWith("PI_"));
        if (prints) this.deps.onTrade?.(prints);
      }
      return;
    }

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastSeq = parsed.seq;
      this.deps.setState("live");
      return;
    }

    if (this.lastSeq === null) return;
    if (parsed.seq === this.lastSeq + 1) {
      this.deps.book.applyUpdates([parsed.update]);
      this.lastSeq = parsed.seq;
    } else if (parsed.seq > this.lastSeq + 1) {
      this.resync(`seq gap: expected ${this.lastSeq + 1}, got ${parsed.seq}`);
    }
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
