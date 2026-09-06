import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import {
  isRecord,
  priceLevel,
  safeInteger,
  type AdapterDeps,
  type VenueAdapter,
} from "./types";

const WS_URL = "wss://advanced-trade-ws.coinbase.com";

export type ParsedCoinbase =
  | { kind: "snapshot"; seq: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "update"; seq: number; updates: L2Update[] }
  | null;

export function parseCoinbaseL2(msg: unknown, sizeMultiplier = 1): ParsedCoinbase {
  if (!isRecord(msg) || msg.channel !== "l2_data") return null;
  const seq = safeInteger(msg.sequence_num);
  if (seq === null || !Array.isArray(msg.events)) return null;

  const bids: PriceLevel[] = [];
  const asks: PriceLevel[] = [];
  const updates: L2Update[] = [];
  let sawSnapshot = false;

  for (const evRaw of msg.events) {
    if (!isRecord(evRaw) || !Array.isArray(evRaw.updates)) return null;
    if (evRaw.type !== "snapshot" && evRaw.type !== "update") return null;
    for (const uRaw of evRaw.updates) {
      const level = toLevel(uRaw, sizeMultiplier);
      if (!level) return null;
      if (evRaw.type === "snapshot") {
        if (level.side === "bid") bids.push({ price: level.price, size: level.size });
        else asks.push({ price: level.price, size: level.size });
      } else {
        updates.push(level);
      }
    }
    if (evRaw.type === "snapshot") sawSnapshot = true;
  }

  if (sawSnapshot) return { kind: "snapshot", seq, bids, asks };
  return { kind: "update", seq, updates };
}

function toLevel(u: unknown, sizeMultiplier: number): L2Update | null {
  if (!isRecord(u)) return null;
  if (u.side !== "bid" && u.side !== "offer") return null;
  if (typeof u.price_level !== "string" || typeof u.new_quantity !== "string") return null;
  const level = priceLevel(u.price_level, u.new_quantity, sizeMultiplier);
  return level ? { side: u.side === "offer" ? "ask" : "bid", ...level } : null;
}

export function parseCoinbaseTrades(msg: unknown, sizeMultiplier = 1): TradePrint[] | null {
  if (!isRecord(msg) || msg.channel !== "market_trades") return null;
  if (!Array.isArray(msg.events)) return null;

  const prints: TradePrint[] = [];
  for (const evRaw of msg.events) {
    if (!isRecord(evRaw)) return null;
    if (evRaw.type !== "snapshot" && evRaw.type !== "update") return null;
    if(evRaw.type==="snapshot") continue;
    if (!Array.isArray(evRaw.trades)) return null;
    for (const tRaw of evRaw.trades) {
      const print = toPrint(tRaw, sizeMultiplier);
      if (!print) return null;
      prints.push(print);
    }
  }
  return prints.length > 0 ? prints : null;
}

function toPrint(t: unknown, sizeMultiplier: number): TradePrint | null {
  if (!isRecord(t)) return null;
  if (typeof t.price !== "string" || typeof t.size !== "string") return null;
  if (t.side !== "BUY" && t.side !== "SELL") return null;
  const ts = typeof t.time === "string" ? Date.parse(t.time) : Number.NaN;
  const level = priceLevel(t.price, t.size, sizeMultiplier);
  if (!level || level.size <= 0) return null;
  return {
    ...level,
    side: t.side === "BUY" ? "buy" : "sell",
    ts: Number.isNaN(ts) ? Date.now() : ts,
  };
}

export interface CoinbaseConfig {
  readonly id: string;
  readonly symbol: string;

  readonly sizeMultiplier?: number;
}

export class CoinbaseL2Adapter implements VenueAdapter {
  readonly id: string;
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly sizeMultiplier: number;
  private ws: WebSocket | null = null;
  private lastSeq: number | null = null;
  private synced=false;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, config: CoinbaseConfig) {
    this.deps = deps;
    this.id = config.id;
    this.symbol = config.symbol;
    this.sizeMultiplier = config.sizeMultiplier ?? 1;
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
    this.lastSeq = null; this.synced=false;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({ type: "subscribe", channel: "level2", product_ids: [this.symbol] }),
      );

      ws.send(
        JSON.stringify({ type: "subscribe", channel: "market_trades", product_ids: [this.symbol] }),
      );

      ws.send(JSON.stringify({ type: "subscribe", channel: "heartbeats" }));
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.conn.dropped("closed");
    ws.onerror = () => this.conn.dropped("socket error");
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
    this.lastSeq = null; this.synced=false;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }

    // sequence_num is shared by every channel on this connection, including
    // subscription acknowledgements, heartbeats and trade messages.
    if(isRecord(raw)) {
      const seq=safeInteger(raw.sequence_num);
      if(seq!==null) {
        if(this.lastSeq!==null && seq!==this.lastSeq+1) {
          this.conn.dropped(`sequence gap: last ${this.lastSeq}, got ${seq}`); return;
        }
        this.lastSeq=seq;
      }
    }
    const prints = parseCoinbaseTrades(raw, this.sizeMultiplier);
    if (prints) {
      this.deps.onTrade?.(prints);
      return;
    }

    const parsed = parseCoinbaseL2(raw, this.sizeMultiplier);
    if (!parsed) {
      if (isRecord(raw) && raw.channel === "heartbeats") {
        this.deps.book.noteActivity();
      } else if (isRecord(raw) && raw.channel === "l2_data") {
        // A malformed l2 batch is discarded unread — the book can no longer
        // be trusted, so force a fresh subscribe + snapshot.
        this.conn.dropped("malformed l2_data frame");
      }
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

export class CoinbaseAdapter extends CoinbaseL2Adapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "coinbase", symbol: "ETH-USD" });
  }
}
