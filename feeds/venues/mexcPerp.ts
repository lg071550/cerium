import type { PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord, safeInteger, type AdapterDeps, type VenueAdapter } from "./types";

const WS_URL = "wss://contract.mexc.com/edge";

const DEPTH_LIMIT = 200;

const CONTRACT_SIZE = 0.01;
const PING_INTERVAL_MS = 20_000;

export interface ParsedMexcPerp {
  version: number;
  bids: PriceLevel[];
  asks: PriceLevel[];
}

export function parseMexcPerp(msg: unknown): ParsedMexcPerp | null {
  if (!isRecord(msg) || msg.channel !== "push.depth.full" || !isRecord(msg.data)) return null;
  const d = msg.data;
  const version = safeInteger(d.version);
  if (version === null) return null;
  const bids = levels(d.bids);
  const asks = levels(d.asks);
  if (!bids || !asks) return null;
  return { version, bids, asks };
}

export function parseMexcDeal(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || msg.channel !== "push.deal" || !Array.isArray(msg.data)) return null;
  const out: TradePrint[] = [];
  for (const row of msg.data) {
    if (!isRecord(row)) return null;
    const side = row.T === 1 ? "buy" : row.T === 2 ? "sell" : null;
    if (side === null) return null;
    if (typeof row.p !== "number" || typeof row.v !== "number" || typeof row.t !== "number") {
      return null;
    }
    if (
      !Number.isFinite(row.p) ||
      !Number.isFinite(row.v) ||
      !Number.isFinite(row.t) ||
      row.p <= 0 ||
      row.v <= 0
    ) return null;
    out.push({ price: row.p, size: row.v * CONTRACT_SIZE, side, ts: row.t });
  }
  return out.length > 0 ? out : null;
}

function levels(v: unknown): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!Array.isArray(row)) return null;
    const price: unknown = row[0];
    const vol: unknown = row[1];
    if (
      typeof price !== "number" ||
      typeof vol !== "number" ||
      !Number.isFinite(price) ||
      !Number.isFinite(vol) ||
      price <= 0 ||
      vol < 0
    ) return null;
    if (vol === 0) continue;
    out.push({ price, size: vol * CONTRACT_SIZE });
  }
  return out;
}

export class MexcPerpAdapter implements VenueAdapter {
  readonly id = "mexc-perp";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private lastVersion = 0;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = "ETH_USDT") {
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
    this.lastVersion = 0;
    const ws = new WebSocket(WS_URL);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({
          method: "sub.depth.full",
          param: { symbol: this.symbol, limit: DEPTH_LIMIT },
        }),
      );
      ws.send(JSON.stringify({ method: "sub.deal", param: { symbol: this.symbol } }));
      this.pingTimer = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ method: "ping" }));
      }, PING_INTERVAL_MS);
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.conn.dropped("closed");
    ws.onerror = () => this.conn.dropped("socket error");
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
    this.lastVersion = 0;
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
    const parsed = parseMexcPerp(raw);
    if (!parsed) {

      const prints = parseMexcDeal(raw);
      if (prints) this.deps.onTrade?.(prints);
      return;
    }
    if (parsed.version <= this.lastVersion) return;
    this.lastVersion = parsed.version;
    this.deps.book.applySnapshot(parsed.bids, parsed.asks);
    this.deps.setState("live");
  }
}
