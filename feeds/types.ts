// Shared types for the feeds worker (trimmed from aggbook's core/types.ts).

export type Side = "bid" | "ask";
export type TradeSide = "buy" | "sell";

export interface PriceLevel {
  price: number;
  size: number;
}

export interface L2Update {
  side: Side;
  price: number;
  size: number;
}

export interface TradePrint {
  price: number;
  size: number;
  side: TradeSide;
  ts: number;
}

export type FeedState = "connecting" | "syncing" | "live" | "reconnecting" | "error";
