import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, priceLevel, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://api.hyperliquid.xyz/ws";
const PING_INTERVAL_MS = 20000;

export type ParsedHyperliquid =
  | { kind: "snapshot"; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "bbo"; bid: PriceLevel | null; ask: PriceLevel | null }
  | null;

export function parseHyperliquid(msg: unknown): ParsedHyperliquid {
  if (!isRecord(msg) || !isRecord(msg.data)) return null;
  const data = msg.data;

  if (msg.channel === "l2Book") {
    const levels = data.levels;
    if (!Array.isArray(levels) || levels.length < 2) return null;
    const bids = parseLevels(levels[0]);
    const asks = parseLevels(levels[1]);
    if (!bids || !asks) return null;
    return { kind: "snapshot", bids, asks };
  }

  if (msg.channel === "bbo") {
    const bbo = data.bbo;
    if (!Array.isArray(bbo) || bbo.length < 2) return null;
    const bid = parseBboEntry(bbo[0]);
    const ask = parseBboEntry(bbo[1]);
    if (bid === undefined || ask === undefined) return null;
    return { kind: "bbo", bid, ask };
  }

  return null;
}

export function parseHyperliquidTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || msg.channel !== "trades") return null;
  if (!Array.isArray(msg.data)) return null;
  const prints: TradePrint[] = [];
  for (const row of msg.data) {
    if (!isRecord(row)) return null;
    if (typeof row.px !== "string" || typeof row.sz !== "string") return null;
    if (row.side !== "B" && row.side !== "A") return null;
    if (typeof row.time !== "number") return null;
    const level = priceLevel(row.px, row.sz);
    if (!level || level.size <= 0 || !Number.isFinite(row.time)) return null;
    prints.push({
      ...level,
      side: row.side === "B" ? "buy" : "sell",
      ts: row.time,
    });
  }
  return prints.length > 0 ? prints : null;
}

export function bboToUpdates(bid: PriceLevel | null, ask: PriceLevel | null): L2Update[] {
  const updates: L2Update[] = [];
  if (bid) updates.push({ side: "bid", ...bid });
  if (ask) updates.push({ side: "ask", ...ask });
  return updates;
}

export function replaceBboUpdates(
  previousBid: PriceLevel | null,
  previousAsk: PriceLevel | null,
  bid: PriceLevel | null,
  ask: PriceLevel | null,
): L2Update[] {
  const updates: L2Update[] = [];
  const replaceSide = (
    side: L2Update["side"],
    previous: PriceLevel | null,
    next: PriceLevel | null,
  ): void => {
    if (previous && (!next || previous.price !== next.price)) {
      updates.push({ side, price: previous.price, size: 0 });
    }
    if (next) updates.push({ side, ...next });
  };
  replaceSide("bid", previousBid, bid);
  replaceSide("ask", previousAsk, ask);
  return updates;
}

function parseLevels(v: unknown): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!isRecord(row) || typeof row.px !== "string" || typeof row.sz !== "string") return null;
    const level = priceLevel(row.px, row.sz);
    if (!level) return null;
    out.push(level);
  }
  return out;
}

function parseBboEntry(v: unknown): PriceLevel | null | undefined {
  if (v === null) return null;
  if (!isRecord(v) || typeof v.px !== "string" || typeof v.sz !== "string") return undefined;
  return priceLevel(v.px, v.sz) ?? undefined;
}

export class HyperliquidAdapter implements VenueAdapter {
  readonly id = "hyperliquid";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private synced = false;
  private overlayBid: PriceLevel | null = null;
  private overlayAsk: PriceLevel | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, coin = "ETH") {
    this.deps = deps;
    this.symbol = coin;
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
      ws.send(JSON.stringify({
        method: "subscribe",
        subscription: { type: "l2Book", coin: this.symbol, nLevels: 100 },
      }));
      ws.send(JSON.stringify({ method: "subscribe", subscription: { type: "bbo", coin: this.symbol } }));
      ws.send(JSON.stringify({ method: "subscribe", subscription: { type: "trades", coin: this.symbol } }));

      this.pingTimer = setInterval(() => {
        if (this.ws?.readyState === WebSocket.OPEN) {
          this.ws.send(JSON.stringify({ method: "ping" }));
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
    this.synced = false;
    this.overlayBid = null;
    this.overlayAsk = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    if (isRecord(raw) && raw.channel === "pong") {
      this.deps.book.noteActivity();
      return;
    }

    const prints = parseHyperliquidTrades(raw);
    if (prints) {
      this.deps.onTrade?.(prints);
      return;
    }

    const parsed = parseHyperliquid(raw);
    if (!parsed) return;

    if (parsed.kind === "snapshot") {

      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      const bestBid = parsed.bids.reduce<PriceLevel | null>(
        (best, level) => (best === null || level.price > best.price ? level : best),
        null,
      );
      const bestAsk = parsed.asks.reduce<PriceLevel | null>(
        (best, level) => (best === null || level.price < best.price ? level : best),
        null,
      );
      this.overlayBid = bestBid;
      this.overlayAsk = bestAsk;
      this.synced = true;
      this.deps.setState("live");
      return;
    }

    if (!this.synced) return;
    this.deps.book.applyUpdates(
      replaceBboUpdates(this.overlayBid, this.overlayAsk, parsed.bid, parsed.ask),
    );
    this.overlayBid = parsed.bid && parsed.bid.size > 0 ? parsed.bid : null;
    this.overlayAsk = parsed.ask && parsed.ask.size > 0 ? parsed.ask : null;
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
