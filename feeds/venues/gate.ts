import type { L2Update, PriceLevel, TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import {
  finiteNumber,
  isRecord,
  priceLevel,
  safeInteger,
  type AdapterDeps,
  type VenueAdapter,
} from "./types";

export type ParsedGate =
  | { kind: "snapshot"; u: number; bids: PriceLevel[]; asks: PriceLevel[] }
  | { kind: "increment"; U: number; u: number; updates: L2Update[] }
  | null;

export function parseGateObu(msg: unknown, sizeMultiplier: number): ParsedGate {
  if (!isRecord(msg) || msg.event !== "update" || !isRecord(msg.result)) return null;
  const r = msg.result;

  if (r.full === true) {

    const bids = levels(r.b, sizeMultiplier);
    const asks = levels(r.a, sizeMultiplier);
    if (!bids || !asks) return null;
    const u = safeInteger(r.u);
    if (u === null) return null;
    return { kind: "snapshot", u, bids, asks };
  }

  const U = safeInteger(r.U);
  const u = safeInteger(r.u);
  if (U === null || u === null) return null;

  const bids = levels(r.b ?? [], sizeMultiplier);
  const asks = levels(r.a ?? [], sizeMultiplier);
  if (!bids || !asks) return null;
  const updates: L2Update[] = [];
  for (const l of bids) updates.push({ side: "bid", ...l });
  for (const l of asks) updates.push({ side: "ask", ...l });
  return { kind: "increment", U, u, updates };
}

export function parseGate(msg: unknown): ParsedGate {
  return parseGateObu(msg, 1);
}

export function parseGateTrades(msg: unknown, sizeMultiplier = 1): TradePrint[] | null {
  if (!isRecord(msg) || msg.event !== "update") return null;
  if (typeof msg.channel !== "string" || !msg.channel.endsWith(".trades")) return null;
  const r = msg.result;

  if (isRecord(r)) {
    const print = spotTrade(r, sizeMultiplier);
    return print ? [print] : null;
  }

  if (Array.isArray(r)) {
    const out: TradePrint[] = [];
    for (const row of r) {
      if (!isRecord(row)) return null;
      const size = row.size;
      if (typeof size !== "number" || !Number.isFinite(size)) return null;
      if (size === 0) continue;
      if (typeof row.price !== "string") return null;
      const ts = msEpoch(row.create_time_ms);
      if (ts === null) return null;
      const level = priceLevel(row.price, Math.abs(size), sizeMultiplier);
      if (!level || level.size <= 0) return null;
      out.push({
        ...level,
        side: size > 0 ? "buy" : "sell",
        ts,
      });
    }
    return out.length > 0 ? out : null;
  }

  return null;
}

function spotTrade(r: Record<string, unknown>, sizeMultiplier: number): TradePrint | null {
  if (r.side !== "buy" && r.side !== "sell") return null;
  if (typeof r.amount !== "string" || typeof r.price !== "string") return null;
  const ts = msEpoch(r.create_time_ms);
  if (ts === null) return null;
  const level = priceLevel(r.price, r.amount, sizeMultiplier);
  return level && level.size > 0 ? { ...level, side: r.side, ts } : null;
}

function msEpoch(v: unknown): number | null {
  return finiteNumber(v);
}

function levels(v: unknown, sizeMultiplier: number): PriceLevel[] | null {
  if (!Array.isArray(v)) return null;
  const out: PriceLevel[] = [];
  for (const row of v) {
    if (!Array.isArray(row) || typeof row[0] !== "string" || typeof row[1] !== "string") {
      return null;
    }
    const level = priceLevel(row[0], row[1], sizeMultiplier);
    if (!level) return null;
    out.push(level);
  }
  return out;
}

export interface GateAdapterOptions {
  id: string;
  symbol: string;
  wsUrl: string;
  channel: string;
  topic: string;

  sizeMultiplier: number;

  tradesChannel?: string;

  tradesPayload?: string[];
}

export class GateBaseAdapter implements VenueAdapter {
  readonly id: string;
  readonly symbol: string;
  private readonly wsUrl: string;
  private readonly channel: string;
  private readonly topic: string;
  private readonly sizeMultiplier: number;
  private readonly tradesChannel?: string;
  private readonly tradesPayload?: string[];

  private readonly deps: AdapterDeps;
  private ws: WebSocket | null = null;
  private lastU: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, opts: GateAdapterOptions) {
    this.deps = deps;
    this.id = opts.id;
    this.symbol = opts.symbol;
    this.wsUrl = opts.wsUrl;
    this.channel = opts.channel;
    this.topic = opts.topic;
    this.sizeMultiplier = opts.sizeMultiplier;
    this.tradesChannel = opts.tradesChannel;
    this.tradesPayload = opts.tradesPayload;
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
    this.lastU = null;
    const ws = new WebSocket(this.wsUrl);
    this.ws = ws;
    ws.onopen = () => {
      this.conn.connected();
      this.deps.setState("syncing");
      ws.send(
        JSON.stringify({
          time: Math.floor(Date.now() / 1000),
          channel: this.channel,
          event: "subscribe",
          payload: [this.topic],
        }),
      );
      if (this.tradesChannel && this.tradesPayload) {
        ws.send(
          JSON.stringify({
            time: Math.floor(Date.now() / 1000),
            channel: this.tradesChannel,
            event: "subscribe",
            payload: this.tradesPayload,
          }),
        );
      }
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
    this.lastU = null;
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    const parsed = parseGateObu(raw, this.sizeMultiplier);
    if (!parsed) {
      this.handleTrades(raw);
      return;
    }

    if (parsed.kind === "snapshot") {
      this.deps.book.applySnapshot(parsed.bids, parsed.asks);
      this.lastU = parsed.u;
      this.deps.setState("live");
      return;
    }

    if (this.lastU === null) return;
    if (parsed.u <= this.lastU) return;
    this.deps.book.applyUpdates(parsed.updates);
    this.lastU = parsed.u;
  }

  private handleTrades(raw: unknown): void {
    if (!this.tradesChannel) return;
    const prints = parseGateTrades(raw, this.sizeMultiplier);
    if (!prints) return;
    this.deps.onTrade?.(prints);
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}

export class GateAdapter extends GateBaseAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "gate",
      symbol: "ETH_USDT",
      wsUrl: "wss://api.gateio.ws/ws/v4/",
      channel: "spot.obu",
      topic: "ob.ETH_USDT.400",
      sizeMultiplier: 1,
      tradesChannel: "spot.trades",
      tradesPayload: ["ETH_USDT"],
    });
  }
}
