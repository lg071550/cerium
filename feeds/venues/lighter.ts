import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import {
  isRecord,
  priceLevel,
  safeInteger,
  type AdapterDeps,
  type VenueAdapter,
} from "./types";

const WS_URL = "wss://mainnet.zklighter.elliot.ai/stream?readonly=true";
const DEFAULT_MARKET_ID = 0;
const PING_INTERVAL_MS = 20_000;

export type ParsedLighter =
  | { kind: "snapshot"; nonce: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; nonce: number; beginNonce: number; updates: L2Update[] }
  | null;

export function parseLighter(msg: unknown): ParsedLighter {
  if (!isRecord(msg) || !isRecord(msg.order_book)) return null;
  const nonce = safeInteger(msg.order_book.nonce);
  if (nonce === null) return null;
  const ob = msg.order_book;

  if (msg.type === "subscribed/order_book") {
    const bids = levels(ob.bids);
    const asks = levels(ob.asks);
    if (!bids || !asks) return null;
    return { kind: "snapshot", nonce, bids, asks };
  }

  if (msg.type === "update/order_book") {
    const beginNonce = safeInteger(ob.begin_nonce);
    if (beginNonce === null) return null;
    const bids = levels(ob.bids);
    const asks = levels(ob.asks);
    if (!bids || !asks) return null;
    const updates: L2Update[] = [];
    for (const l of bids) updates.push({ side: "bid", ...l });
    for (const l of asks) updates.push({ side: "ask", ...l });
    return { kind: "update", nonce, beginNonce, updates };
  }

  return null;
}

function levels(v: unknown): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!isRecord(row) || typeof row.price !== "string" || typeof row.size !== "string") return null;
    const level = priceLevel(row.price, row.size);
    if (!level) return null;
    out.push(level);
  }
  return out;
}

export function parseLighterTrades(
  msg: unknown,
  channel = `trade:${DEFAULT_MARKET_ID}`,
): TradePrint[] | null {
  if (!isRecord(msg) || msg.channel !== channel) return null;
  if (msg.type !== "subscribed/trade" && msg.type !== "update/trade") return null;
  if (!Array.isArray(msg.trades)) return null;
  const prints: TradePrint[] = [];
  for (const row of msg.trades) {
    if (!isRecord(row)) return null;
    if (typeof row.price !== "string" || typeof row.size !== "string") return null;
    if (typeof row.is_maker_ask !== "boolean") return null;
    if (typeof row.timestamp !== "number") return null;
    const level = priceLevel(row.price, row.size);
    if (!level || level.size <= 0 || !Number.isFinite(row.timestamp)) return null;
    prints.push({
      ...level,
      side: row.is_maker_ask ? "buy" : "sell",
      ts: row.timestamp,
    });
  }
  return prints.length > 0 ? prints : null;
}

export class LighterAdapter implements VenueAdapter {
  readonly id = "lighter";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly channel: string;
  private readonly tradesChannel: string;
  private readonly tradesFeed: string;
  private ws: WebSocket | null = null;
  private lastNonce: number | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, marketId = DEFAULT_MARKET_ID, symbol = "ETH") {
    this.deps = deps;
    this.symbol = symbol;
    this.channel = `order_book/${marketId}`;
    this.tradesChannel = `trade/${marketId}`;
    this.tradesFeed = `trade:${marketId}`;
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
    this.lastNonce = null;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(JSON.stringify({ type: "subscribe", channel: this.channel }));
      ws.send(JSON.stringify({ type: "subscribe", channel: this.tradesChannel }));
      this.pingTimer = setInterval(() => {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
          this.ws.send(JSON.stringify({ type: "ping" }));
        }
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
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }
    this.deps.book.clear();
    this.lastNonce = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    if (isRecord(raw) && raw.type === "pong") {
      this.deps.book.noteActivity();
      return;
    }

    const prints = parseLighterTrades(raw, this.tradesFeed);
    if (prints) {
      this.deps.onTrade?.(prints);
      return;
    }

    const parsed = parseLighter(raw);
    if (!parsed) return;

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastNonce = parsed.nonce;
      this.deps.setState("live");
      return;
    }

    if (this.lastNonce === null) return;

    if (parsed.beginNonce !== this.lastNonce) {
      this.resync(`nonce gap: expected begin_nonce ${this.lastNonce}, got ${parsed.beginNonce}`);
      return;
    }
    this.deps.book.applyUpdates(parsed.updates);
    this.lastNonce = parsed.nonce;
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
