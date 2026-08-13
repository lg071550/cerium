// Vendored from aggbook (src/venues/binanceTrades.ts) — aggTrade/trade socket.
// Only import paths changed.

import type { TradePrint } from "../types";
import { Reconnect } from "./reconnect";
import { isRecord } from "./types";

export function parseAggTrade(msg: unknown): TradePrint[] | null {
  if (!isRecord(msg) || (msg.e !== "aggTrade" && msg.e !== "trade")) return null;
  if (typeof msg.p !== "string" || typeof msg.q !== "string") return null;
  if (typeof msg.m !== "boolean" || typeof msg.T !== "number") return null;
  const price = Number(msg.p);
  const size = Number(msg.q);
  if (!Number.isFinite(price) || price <= 0 || !Number.isFinite(size) || size <= 0)
    return null;

  return [{ price, size, side: msg.m ? "sell" : "buy", ts: msg.T }];
}

export class AggTradeSocket {
  private ws: WebSocket | null = null;
  private readonly conn: Reconnect;

  constructor(
    private readonly url: string,
    private readonly onTrade?: (prints: TradePrint[]) => void,
  ) {
    this.conn = new Reconnect(
      () => {},
      () => this.open(),
      () => this.teardown(),
    );
  }

  start(): void {
    this.conn.start();
  }

  stop(): void {
    this.conn.stop();
  }

  private open(): void {
    const ws = new WebSocket(this.url);
    this.ws = ws;
    ws.onopen = () => this.conn.connected();
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.conn.dropped("tape closed");
    ws.onerror = () => this.conn.dropped("tape socket error");
  }

  private teardown(): void {
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    const prints = parseAggTrade(raw);
    if (prints) this.onTrade?.(prints);
  }
}
