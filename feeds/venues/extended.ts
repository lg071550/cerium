import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import {
  isRecord,
  priceLevel,
  safeInteger,
  type AdapterDeps,
  type VenueAdapter,
} from "./types";

const WS_BASE = "wss://api.starknet.extended.exchange/stream.extended.exchange/v1";

export type ParsedExtended =
  | { kind: "snapshot"; seq: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "delta"; seq: number; updates: L2Update[] }
  | null;

export function parseExtended(msg: unknown): ParsedExtended {
  if (!isRecord(msg) || !isRecord(msg.data)) return null;
  const seq = safeInteger(msg.seq);
  if (seq === null) return null;
  const d = msg.data;

  if (msg.type === "SNAPSHOT") {
    const bids = levels(d.b, false);
    const asks = levels(d.a, false);
    if (!bids || !asks) return null;
    return { kind: "snapshot", seq, bids, asks };
  }

  if (msg.type === "DELTA") {
    const bids = levels(d.b, true);
    const asks = levels(d.a, true);
    if (!bids || !asks) return null;
    const updates: L2Update[] = [];
    for (const l of bids) updates.push({ side: "bid", ...l });
    for (const l of asks) updates.push({ side: "ask", ...l });
    return { kind: "delta", seq, updates };
  }

  return null;
}

function levels(v: unknown, delta: boolean): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!isRecord(row) || typeof row.p !== "string") return null;
    const sizeField = delta ? row.c : row.q;
    if (typeof sizeField !== "string") return null;
    const level = priceLevel(row.p, sizeField);
    if (!level) return null;
    out.push(level);
  }
  return out;
}

export function parseExtendedTrades(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || !Array.isArray(msg.data)) return null;
  const prints: TradePrint[] = [];
  for (const row of msg.data) {
    if (!isRecord(row)) return null;
    if (typeof row.p !== "string" || typeof row.q !== "string") return null;
    if (row.S !== "BUY" && row.S !== "SELL") return null;
    if (typeof row.T !== "number") return null;
    const level = priceLevel(row.p, row.q);
    if (!level || level.size <= 0 || !Number.isFinite(row.T)) return null;
    prints.push({
      ...level,
      side: row.S === "BUY" ? "buy" : "sell",
      ts: row.T,
    });
  }
  return prints.length > 0 ? prints : null;
}

export class ExtendedAdapter implements VenueAdapter {
  readonly id = "extended";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly wsUrl: string;
  private readonly tradesWsUrl: string;
  private ws: WebSocket | null = null;
  private lastSeq: number | null = null;
  private readonly conn: Reconnect;
  private tradeWs: WebSocket | null = null;
  private readonly tradeConn: Reconnect;

  constructor(deps: AdapterDeps, inst = "ETH-USD") {
    this.deps = deps;
    this.symbol = inst;
    this.wsUrl = `${WS_BASE}/orderbooks/${inst}`;
    this.tradesWsUrl = `${WS_BASE}/publicTrades/${inst}`;
    this.conn = new Reconnect(
      deps.setState,
      () => this.open(),
      () => this.teardown(),
    );
    this.tradeConn = new Reconnect(

      () => {},
      () => this.openTrades(),
      () => this.teardownTrades(),
    );
  }

  start(): void {
    this.conn.start();
    this.tradeConn.start();
  }

  stop(): void {
    this.conn.stop();
    this.tradeConn.stop();
    this.deps.book.clear();
  }

  private open(): void {
    this.deps.setState("connecting");
    this.lastSeq = null;
    const ws = new WebSocket(this.wsUrl);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
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
    this.lastSeq = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    const parsed = parseExtended(raw);
    if (!parsed) return;

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastSeq = parsed.seq;
      this.deps.setState("live");
      return;
    }

    if (this.lastSeq === null) return;
    if (parsed.seq === this.lastSeq + 1) {
      this.deps.book.applyUpdates(parsed.updates);
      this.lastSeq = parsed.seq;
    } else if (parsed.seq > this.lastSeq + 1) {
      this.resync(`seq gap: expected ${this.lastSeq + 1}, got ${parsed.seq}`);
    }
  }

  private openTrades(): void {
    const ws = new WebSocket(this.tradesWsUrl);
    this.tradeWs = ws;
    ws.onopen = () => {
      this.tradeConn.connected();
    };
    ws.onmessage = (e) => this.handleTrade(e);
    ws.onclose = () => this.tradeConn.dropped("trades socket closed");
    ws.onerror = () => this.tradeConn.dropped("trades socket error");
  }

  private teardownTrades(): void {
    if (this.tradeWs) {
      this.tradeWs.onclose = null;
      this.tradeWs.onerror = null;
      this.tradeWs.onmessage = null;
      this.tradeWs.close();
      this.tradeWs = null;
    }
  }

  private handleTrade(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    const prints = parseExtendedTrades(raw);
    if (prints) this.deps.onTrade?.(prints);
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
