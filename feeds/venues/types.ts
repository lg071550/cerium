// Adapter contract for the feeds worker. Identical surface to aggbook's
// AdapterDeps, but the book is a sink interface — the wire emits binary events
// instead of maintaining a local L2Book (books live in WASM now).

import type { FeedState, L2Update, PriceLevel, TradePrint } from "../types";

export interface BookSink {
  clear(): void;
  applySnapshot(bids: readonly PriceLevel[], asks: readonly PriceLevel[]): void;
  applyUpdates(updates: readonly L2Update[]): void;
}

export interface AdapterDeps {
  book: BookSink;
  setState: (state: FeedState, detail?: string) => void;
  onTrade?: (prints: TradePrint[]) => void;
}

export interface VenueAdapter {
  readonly id: string;
  readonly symbol: string;
  start(): void;
  stop(): void;
}

export function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

export function finiteNumber(v: unknown): number | null {
  if (typeof v === "number") return Number.isFinite(v) ? v : null;
  if (typeof v !== "string" || v.trim() === "") return null;
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
}

export function safeInteger(v: unknown): number | null {
  return typeof v === "number" && Number.isSafeInteger(v) ? v : null;
}

export function priceLevel(
  priceValue: unknown,
  sizeValue: unknown,
  multiplier = 1,
): PriceLevel | null {
  const price = finiteNumber(priceValue);
  const rawSize = finiteNumber(sizeValue);
  const size = rawSize === null ? null : rawSize * multiplier;
  if (
    price === null ||
    price <= 0 ||
    size === null ||
    !Number.isFinite(size) ||
    size < 0 ||
    !Number.isFinite(multiplier) ||
    multiplier <= 0
  ) {
    return null;
  }
  return { price, size };
}
