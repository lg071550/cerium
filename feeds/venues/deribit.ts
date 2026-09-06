import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, safeInteger, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://www.deribit.com/ws/api/v2";
const DEFAULT_INST = "ETH-PERPETUAL";

export type ParsedDeribit =
  | { kind: "snapshot"; changeId: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "change"; changeId: number; prevChangeId: number; updates: L2Update[] }
  | null;

export function parseDeribit(msg: unknown, inverse = true): ParsedDeribit {
  if (!isRecord(msg) || msg.method !== "subscription" || !isRecord(msg.params)) return null;
  const d = msg.params.data;
  if (!isRecord(d)) return null;
  const changeId = safeInteger(d.change_id);
  if (changeId === null) return null;

  if (d.type === "snapshot") {
    const bids = rows(d.bids, inverse);
    const asks = rows(d.asks, inverse);
    if (!bids || !asks) return null;
    return { kind: "snapshot", changeId, bids, asks };
  }

  if (d.type === "change") {
    const prevChangeId = safeInteger(d.prev_change_id);
    if (prevChangeId === null) return null;
    const bids = rows(d.bids, inverse);
    const asks = rows(d.asks, inverse);
    if (!bids || !asks) return null;
    const updates: L2Update[] = [];
    for (const l of bids) updates.push({ side: "bid", ...l });
    for (const l of asks) updates.push({ side: "ask", ...l });
    return { kind: "change", changeId, prevChangeId, updates };
  }

  return null;
}

export function parseDeribitTestRequest(msg: unknown): { id: number } | null {
  if (!isRecord(msg) || msg.method !== "test_request") return null;
  return { id: typeof msg.id === "number" ? msg.id : 0 };
}

export function parseDeribitTrades(
  msg: unknown,
  channel = `trades.${DEFAULT_INST}.100ms`,
  inverse = true,
): TradePrint[] | null {
  if (!isRecord(msg) || msg.method !== "subscription" || !isRecord(msg.params)) return null;
  if (msg.params.channel !== channel) return null;
  if (!Array.isArray(msg.params.data)) return null;
  const prints: TradePrint[] = [];
  for (const row of msg.params.data) {
    if (!isRecord(row)) return null;
    if (typeof row.price !== "number" || !Number.isFinite(row.price) || row.price <= 0) return null;
    if (typeof row.amount !== "number" || !Number.isFinite(row.amount) || row.amount <= 0) return null;
    if (row.direction !== "buy" && row.direction !== "sell") return null;
    if (typeof row.timestamp !== "number" || !Number.isFinite(row.timestamp)) return null;
    prints.push({
      price: row.price,
      size: inverse ? row.amount / row.price : row.amount,
      side: row.direction,
      ts: row.timestamp,
    });
  }
  return prints.length > 0 ? prints : null;
}

function rows(v: unknown, inverse: boolean): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!Array.isArray(row)) return null;
    const action: unknown = row[0];
    const price: unknown = row[1];
    const amount: unknown = row[2];
    if (action !== "new" && action !== "change" && action !== "delete") return null;
    if (
      typeof price !== "number" ||
      !Number.isFinite(price) ||
      typeof amount !== "number" ||
      !Number.isFinite(amount) ||
      price <= 0 ||
      amount < 0
    ) return null;
    out.push({ price, size: action === "delete" ? 0 : inverse ? amount / price : amount });
  }
  return out;
}

export class DeribitAdapter implements VenueAdapter {
  readonly id = "deribit";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly channel: string;
  private readonly tradesChannel: string;
  private ws: WebSocket | null = null;
  private lastChangeId: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = DEFAULT_INST) {
    this.deps = deps;
    this.symbol = inst;
    this.channel = `book.${inst}.100ms`;
    this.tradesChannel = `trades.${inst}.100ms`;
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
    this.lastChangeId = null;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({
          jsonrpc: "2.0",
          id: 1,
          method: "public/subscribe",
          params: { channels: [this.channel] },
        }),
      );
      ws.send(
        JSON.stringify({
          jsonrpc: "2.0",
          id: 2,
          method: "public/subscribe",
          params: { channels: [this.tradesChannel] },
        }),
      );
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
    this.lastChangeId = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }

    const test = parseDeribitTestRequest(raw);
    if (test) {
      this.deps.book.noteActivity();
      this.ws?.send(
        JSON.stringify({ jsonrpc: "2.0", id: test.id, method: "public/test", params: {} }),
      );
      return;
    }

    const prints = parseDeribitTrades(raw, this.tradesChannel, !this.symbol.includes("_USDC"));
    if (prints) {
      this.deps.onTrade?.(prints);
      return;
    }

    const parsed = parseDeribit(raw, !this.symbol.includes("_USDC"));
    if (!parsed) return;

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastChangeId = parsed.changeId;
      this.deps.setState("live");
      return;
    }

    if (this.lastChangeId === null) return;
    if (parsed.prevChangeId === this.lastChangeId) {
      this.deps.book.applyUpdates(parsed.updates);
      this.lastChangeId = parsed.changeId;
    } else {
      this.resync(
        `change_id gap: expected prev ${this.lastChangeId}, got ${parsed.prevChangeId}`,
      );
    }
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
