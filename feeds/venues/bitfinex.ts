import type { FeedId, L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://api-pub.bitfinex.com/ws/2";

export type ParsedBitfinex =
  | { kind: "snapshot"; chanId: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; chanId: number; update: L2Update }
  | null;

export function parseBitfinex(msg: unknown): ParsedBitfinex {
  if (!Array.isArray(msg)) return null;
  const chanId: unknown = msg[0];
  if (typeof chanId !== "number") return null;
  const body: unknown = msg[1];
  if (!Array.isArray(body)) return null;

  if (body.length > 0 && Array.isArray(body[0])) {
    const bids: PriceLevel[] = [];
    const asks: PriceLevel[] = [];
    for (const row of body) {
      const u = parseRow(row);
      if (!u) return null;

      if (u.size === 0) continue;
      (u.side === "bid" ? bids : asks).push({ price: u.price, size: u.size });
    }
    return { kind: "snapshot", chanId, bids, asks };
  }

  const u = parseRow(body);
  if (!u) return null;
  return { kind: "update", chanId, update: u };
}

function parseRow(row: unknown): L2Update | null {
  if (!Array.isArray(row)) return null;
  const price: unknown = row[0];
  const count: unknown = row[1];
  const amount: unknown = row[2];
  if (typeof price !== "number" || typeof count !== "number" || typeof amount !== "number") {
    return null;
  }
  if (amount > 0) return { side: "bid", price, size: count === 0 ? 0 : amount };
  if (amount < 0) return { side: "ask", price, size: count === 0 ? 0 : -amount };
  return null;
}

export function parseBitfinexTrades(msg: unknown): TradePrint[] | null {
  if (!Array.isArray(msg)) return null;
  if (msg[1] !== "te") return null;
  const row: unknown = msg[2];
  if (!Array.isArray(row)) return null;
  const ms: unknown = row[1];
  const amount: unknown = row[2];
  const price: unknown = row[3];
  if (typeof ms !== "number" || typeof amount !== "number" || typeof price !== "number") return null;
  if (amount > 0) return [{ price, size: amount, side: "buy", ts: ms }];
  if (amount < 0) return [{ price, size: -amount, side: "sell", ts: ms }];
  return null;
}

export interface BitfinexAdapterOptions {
  id: FeedId;

  symbol: string;
}

export class BitfinexBookAdapter implements VenueAdapter {
  readonly id: FeedId;
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private chanId: number | null = null;

  private tradesChanId: number | null = null;
  private synced = false;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, opts: BitfinexAdapterOptions) {
    this.deps = deps;
    this.id = opts.id;
    this.symbol = opts.symbol;
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
    this.chanId = null;
    this.tradesChanId = null;
    this.synced = false;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;

    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({
          event: "subscribe",
          channel: "book",
          symbol: this.symbol,
          prec: "P0",
          freq: "F0",
          len: "250",
        }),
      );

      ws.send(JSON.stringify({ event: "subscribe", channel: "trades", symbol: this.symbol }));
    };

    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.resync("closed");
    ws.onerror = () => this.resync("socket error");
  }

  private teardown(): void {
    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }
    this.deps.book.clear();
    this.chanId = null;
    this.tradesChanId = null;
    this.synced = false;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }

    if (isRecord(raw)) {
      if (raw.event === "subscribed" && typeof raw.chanId === "number") {
        if (raw.channel === "book") this.chanId = raw.chanId;
        else if (raw.channel === "trades") this.tradesChanId = raw.chanId;
      }
      return;
    }

    if (Array.isArray(raw) && raw[1] === "hb") {
      this.deps.book.noteActivity();
      return;
    }

    if (Array.isArray(raw) && raw[1] === "te") {
      if (this.tradesChanId !== null && raw[0] === this.tradesChanId) {
        const prints = parseBitfinexTrades(raw);
        if (prints) this.deps.onTrade?.(prints);
      }
      return;
    }

    const parsed = parseBitfinex(raw);
    if (!parsed) return;
    if (this.chanId !== null && parsed.chanId !== this.chanId) return;

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.synced = true;
      this.deps.setState("live");
      return;
    }

    if (!this.synced) return;
    this.deps.book.applyUpdates([parsed.update]);
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}

export class BitfinexAdapter extends BitfinexBookAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "bitfinex", symbol: "tETHUSD" });
  }
}
