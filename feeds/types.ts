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

export type FeedClass = "cex-spot" | "cex-perp" | "dex-perp";

export type FeedId =
  | "coinbase"
  | "binance"
  | "okx"
  | "bybit"
  | "kraken"
  | "bitget"
  | "gate"
  | "bitstamp"
  | "cryptocom"
  | "bitfinex"
  | "coinbase-perp"
  | "coinbase-us-perp"
  | "binance-perp"
  | "okx-perp"
  | "bybit-perp"
  | "kraken-perp"
  | "bitget-perp"
  | "gate-perp"
  | "mexc-perp"
  | "cryptocom-perp"
  | "bitfinex-perp"
  | "deribit"
  | "hyperliquid"
  | "lighter"
  | "extended"
  | "dydx"
  | "aster";

export interface FeedDescriptor {
  id: FeedId;
  label: string;
  venue: string;
  class: FeedClass;
}
