import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, priceLevel, safeInteger, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://stream.crypto.com/exchange/v1/market";
const DEPTH = 150;

export type ParsedCryptocom =
  | { kind: "snapshot"; seq: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "delta"; seq: number; prevSeq: number; updates: L2Update[] }
  | null;

export function parseCryptocom(msg: unknown): ParsedCryptocom {
  if (!isRecord(msg) || msg.method !== "subscribe" || !isRecord(msg.result)) return null;
  const result = msg.result;
  if ((result.channel !== "book" && result.channel !== "book.update") || !Array.isArray(result.data)) {
    return null;
  }
  const d = result.data[0];
  if (!isRecord(d)) return null;
  const seq = safeInteger(d.u);
  if (seq === null) return null;

  if (isRecord(d.update)) {
    const prevSeq = safeInteger(d.pu);
    if (prevSeq === null) return null;
    const bids = levels(d.update.bids);
    const asks = levels(d.update.asks);
    if (!bids || !asks) return null;
    const updates: L2Update[] = [];
    for (const l of bids) updates.push({ side: "bid", ...l });
    for (const l of asks) updates.push({ side: "ask", ...l });
    return { kind: "delta", seq, prevSeq, updates };
  }

  const bids = levels(d.bids);
  const asks = levels(d.asks);
  if (!bids || !asks) return null;
  return { kind: "snapshot", seq, bids, asks };
}

export function parseCryptocomTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || msg.method !== "subscribe" || !isRecord(msg.result)) return null;
  const result = msg.result;
  if (result.channel !== "trade" || !Array.isArray(result.data)) return null;
  const out: TradePrint[] = [];
  for (const row of result.data) {
    if (!isRecord(row)) return null;
    if (typeof row.p !== "string" || typeof row.q !== "string") return null;
    if (row.s !== "BUY" && row.s !== "SELL") return null;
    if (typeof row.t !== "number") return null;
    const level = priceLevel(row.p, row.q);
    if (!level || level.size <= 0 || !Number.isFinite(row.t)) return null;
    out.push({ ...level, side: row.s === "BUY" ? "buy" : "sell", ts: row.t });
  }
  return out.length > 0 ? out : null;
}

export function parseHeartbeat(msg: unknown): number | null {
  if (!isRecord(msg) || msg.method !== "public/heartbeat" || typeof msg.id !== "number") return null;
  return msg.id;
}

function levels(v: unknown): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!Array.isArray(row) || typeof row[0] !== "string" || typeof row[1] !== "string") return null;
    const level = priceLevel(row[0], row[1]);
    if (!level) return null;
    out.push(level);
  }
  return out;
}

export interface CryptocomAdapterOptions {
  id: string;
  symbol: string;
  channel: string;
}

export class CryptocomBaseAdapter implements VenueAdapter {
  readonly id: string;
  readonly symbol: string;
  private readonly channel: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private lastSeq: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, opts: CryptocomAdapterOptions) {
    this.deps = deps;
    this.id = opts.id;
    this.symbol = opts.symbol;
    this.channel = opts.channel;
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
          id: 1,
          method: "subscribe",
          nonce: Date.now(),
          params: {
            channels: [this.channel],
            book_subscription_type: "SNAPSHOT_AND_UPDATE",
            book_update_frequency: "100",
          },
        }),
      );

      ws.send(
        JSON.stringify({
          id: 2,
          method: "subscribe",
          nonce: Date.now(),
          params: { channels: [`trade.${this.symbol}`] },
        }),
      );
    };

    ws.onmessage = (event) => this.handle(event);
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
    this.lastSeq = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }

    const heartbeatId = parseHeartbeat(raw);
    if (heartbeatId !== null) {

      this.deps.book.noteActivity();
      this.ws?.send(JSON.stringify({ id: heartbeatId, method: "public/respond-heartbeat" }));
      return;
    }

    const parsed = parseCryptocom(raw);
    if (!parsed) {

      const prints = parseCryptocomTrades(raw);
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
    if (parsed.seq <= this.lastSeq) return;
    if (parsed.prevSeq !== this.lastSeq) {
      this.resync(`seq gap: expected pu=${this.lastSeq}, got ${parsed.prevSeq}`);
      return;
    }
    this.deps.book.applyUpdates(parsed.updates);
    this.lastSeq = parsed.seq;
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}

export class CryptocomAdapter extends CryptocomBaseAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "cryptocom",
      symbol: "ETH_USD",
      channel: `book.ETH_USD.${DEPTH}`,
    });
  }
}
