import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, priceLevel, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://indexer.dydx.trade/v4/ws";

export type ParsedDydx =
  | { kind: "snapshot"; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "delta"; updates: L2Update[] }
  | null;

export function parseDydx(msg: unknown): ParsedDydx {
  if (!isRecord(msg) || msg.channel !== "v4_orderbook") return null;

  if (msg.type === "subscribed") {
    if (!isRecord(msg.contents)) return null;
    const bids = objectLevels(msg.contents.bids);
    const asks = objectLevels(msg.contents.asks);
    if (!bids || !asks) return null;
    return { kind: "snapshot", bids, asks };
  }

  if (msg.type === "channel_batch_data") {
    if (!Array.isArray(msg.contents)) return null;
    const updates: L2Update[] = [];
    for (const tick of msg.contents) {
      if (!isRecord(tick)) return null;
      const bids = pairLevels(tick.bids);
      const asks = pairLevels(tick.asks);
      if (!bids || !asks) return null;
      for (const l of bids) updates.push({ side: "bid", ...l });
      for (const l of asks) updates.push({ side: "ask", ...l });
    }
    return { kind: "delta", updates };
  }

  return null;
}

function objectLevels(v: unknown): PriceLevel[] | null {
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

function pairLevels(v: unknown): PriceLevel[] | null {
  if (v === undefined) return [];
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!Array.isArray(row) || row.length !== 2) return null;
    const price: unknown = row[0];
    const size: unknown = row[1];
    if (typeof price !== "string" || typeof size !== "string") return null;
    const level = priceLevel(price, size);
    if (!level) return null;
    out.push(level);
  }
  return out;
}

export function parseDydxTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || msg.channel !== "v4_trades") return null;
  if (msg.type !== "subscribed" && msg.type !== "channel_data" && msg.type !== "channel_batch_data") {
    return null;
  }
  const contents = msg.contents;
  const ticks: unknown[] = isRecord(contents) ? [contents] : Array.isArray(contents) ? contents : [];
  const prints: TradePrint[] = [];
  for (const tick of ticks) {
    if (!isRecord(tick)) return null;
    if (!Array.isArray(tick.trades)) return null;
    for (const row of tick.trades) {
      if (!isRecord(row)) return null;
      if (typeof row.price !== "string" || typeof row.size !== "string") return null;
      if (row.side !== "BUY" && row.side !== "SELL") return null;
      const parsed = typeof row.createdAt === "string" ? Date.parse(row.createdAt) : Number.NaN;
      const level = priceLevel(row.price, row.size);
      if (!level || level.size <= 0) return null;
      prints.push({
        ...level,
        side: row.side === "BUY" ? "buy" : "sell",
        ts: Number.isNaN(parsed) ? Date.now() : parsed,
      });
    }
  }
  return prints.length > 0 ? prints : null;
}

export class DydxAdapter implements VenueAdapter {
  readonly id = "dydx";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private synced = false;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = "ETH-USD") {
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
          type: "subscribe",
          channel: "v4_orderbook",
          id: this.symbol,
          batched: true,
        }),
      );
      ws.send(
        JSON.stringify({
          type: "subscribe",
          channel: "v4_trades",
          id: this.symbol,
          batched: true,
        }),
      );
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.resync("closed");
    ws.onerror = () => this.resync("socket error");
  }

  private teardown(): void {
    if (this.ws) {
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
      this.resync("invalid JSON frame");
      return;
    }

    const prints = parseDydxTrades(raw);
    if (prints) {
      this.deps.onTrade?.(prints);
      return;
    }

    const parsed = parseDydx(raw);
    if (!parsed) {

      if (
        isRecord(raw) &&
        raw.channel === "v4_orderbook" &&
        (raw.type === "subscribed" || raw.type === "channel_batch_data")
      ) {
        this.resync("unparseable book message");
      }
      return;
    }

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.synced = true;
      this.deps.setState("live");
      return;
    }

    if (!this.synced) {

      this.resync("delta before snapshot");
      return;
    }
    this.deps.book.applyUpdates(parsed.updates);
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
