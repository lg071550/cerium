// Vendored from aggbook (src/venues/binanceSync.ts) — Binance snapshot/diff
// reconcile state machine. Only import paths changed.

import type { L2Update, PriceLevel } from "../types";
import { isRecord, priceLevel } from "./types";

export interface BinanceSyncState {
  updateId: number;
  mode: "buffering" | "bridging" | "live";
}

export type FuturesSyncState = BinanceSyncState;

export interface DepthEvent {
  U: number;
  u: number;
  b: [string, string][];
  a: [string, string][];
}

export interface FuturesDepthEvent {
  U: number;
  u: number;
  pu: number;
  b: [string, string][];
  a: [string, string][];
}

export interface BinanceSnapshot {
  lastUpdateId: number;
  bids: [string, string][];
  asks: [string, string][];
}

export type FuturesSnapshot = BinanceSnapshot;

export type SyncAction =
  | { kind: "applySnapshot"; bids: PriceLevel[]; asks: PriceLevel[]; updateId: number }
  | { kind: "applyUpdates"; updates: L2Update[]; updateId: number }
  | { kind: "skip" }
  | { kind: "resync" };

export type FuturesSyncAction = SyncAction;

export interface ReconcileSnapshotResult {
  actions: SyncAction[];
  newState: BinanceSyncState;
}

export interface ReconcileEventResult {
  action: SyncAction;
  newState: BinanceSyncState;
}

function parsePairs(v: unknown): [string, string][] | null {
  if (!Array.isArray(v)) return null;
  const out: [string, string][] = [];
  for (const row of v) {
    if (!Array.isArray(row)) return null;
    const p: unknown = row[0];
    const q: unknown = row[1];
    if (typeof p !== "string" || typeof q !== "string" || !priceLevel(p, q)) return null;
    out.push([p, q]);
  }
  return out;
}

export function parseDepthMessage(raw: unknown): DepthEvent | null {
  if (!isRecord(raw) || raw.e !== "depthUpdate") return null;
  const U = raw.U;
  const u = raw.u;
  if (
    typeof U !== "number" ||
    typeof u !== "number" ||
    !Number.isSafeInteger(U) ||
    !Number.isSafeInteger(u)
  ) {
    return null;
  }
  const b = parsePairs(raw.b);
  const a = parsePairs(raw.a);
  return b && a ? { U, u, b, a } : null;
}

export function parseFuturesDepthMessage(raw: unknown): FuturesDepthEvent | null {
  if (!isRecord(raw) || raw.e !== "depthUpdate") return null;
  const U = raw.U;
  const u = raw.u;
  const pu = raw.pu;
  if (
    typeof U !== "number" ||
    typeof u !== "number" ||
    typeof pu !== "number" ||
    !Number.isSafeInteger(U) ||
    !Number.isSafeInteger(u) ||
    !Number.isSafeInteger(pu)
  ) {
    return null;
  }
  const b = parsePairs(raw.b);
  const a = parsePairs(raw.a);
  return b && a ? { U, u, pu, b, a } : null;
}

export function parseSnapshotResponse(raw: unknown): BinanceSnapshot | null {
  if (
    !isRecord(raw) ||
    typeof raw.lastUpdateId !== "number" ||
    !Number.isSafeInteger(raw.lastUpdateId)
  ) {
    return null;
  }
  const bids = parsePairs(raw.bids);
  const asks = parsePairs(raw.asks);
  return bids && asks ? { lastUpdateId: raw.lastUpdateId, bids, asks } : null;
}

function toLevels(rows: [string, string][]): PriceLevel[] {
  const out: PriceLevel[] = [];
  for (const [p, q] of rows) out.push({ price: Number(p), size: Number(q) });
  return out;
}

function toUpdates(ev: DepthEvent | FuturesDepthEvent): L2Update[] {
  const out: L2Update[] = [];
  for (const [p, q] of ev.b) out.push({ side: "bid", price: Number(p), size: Number(q) });
  for (const [p, q] of ev.a) out.push({ side: "ask", price: Number(p), size: Number(q) });
  return out;
}

export function reconcileSnapshot(
  state: BinanceSyncState,
  buffer: DepthEvent[],
  snap: BinanceSnapshot,
): ReconcileSnapshotResult {
  const snapLevels = {
    bids: toLevels(snap.bids),
    asks: toLevels(snap.asks),
  };

  if (buffer.length === 0) {
    return {
      actions: [{ kind: "applySnapshot", ...snapLevels, updateId: snap.lastUpdateId }],
      newState: { updateId: snap.lastUpdateId, mode: "live" },
    };
  }

  const first = buffer[0];
  if (first && snap.lastUpdateId < first.U) {
    return { actions: [{ kind: "resync" }], newState: state };
  }

  const remaining = buffer.filter((ev) => ev.u > snap.lastUpdateId);
  if (remaining.length === 0) {
    return {
      actions: [{ kind: "applySnapshot", ...snapLevels, updateId: snap.lastUpdateId }],
      newState: { updateId: snap.lastUpdateId, mode: "live" },
    };
  }
  const head = remaining[0];
  if (head && head.U > snap.lastUpdateId + 1) {
    return { actions: [{ kind: "resync" }], newState: state };
  }

  const actions: SyncAction[] = [
    { kind: "applySnapshot", ...snapLevels, updateId: snap.lastUpdateId },
  ];
  let updateId = snap.lastUpdateId;
  for (const ev of remaining) {
    actions.push({ kind: "applyUpdates", updates: toUpdates(ev), updateId: ev.u });
    updateId = ev.u;
  }
  return { actions, newState: { updateId, mode: "live" } };
}

export function reconcileEvent(
  state: BinanceSyncState,
  ev: DepthEvent,
): ReconcileEventResult {
  if (ev.u < state.updateId) {
    return { action: { kind: "skip" }, newState: state };
  }
  if (ev.U > state.updateId + 1) {
    return { action: { kind: "resync" }, newState: state };
  }
  return {
    action: { kind: "applyUpdates", updates: toUpdates(ev), updateId: ev.u },
    newState: { updateId: ev.u, mode: "live" },
  };
}

export function reconcileFuturesSnapshot(
  state: FuturesSyncState,
  buffer: FuturesDepthEvent[],
  snap: FuturesSnapshot,
): ReconcileSnapshotResult {
  const snapLevels = { bids: toLevels(snap.bids), asks: toLevels(snap.asks) };

  if (buffer.length === 0) {
    return {
      actions: [{ kind: "applySnapshot", ...snapLevels, updateId: snap.lastUpdateId }],
      newState: { updateId: snap.lastUpdateId, mode: "bridging" },
    };
  }

  const remaining = buffer.filter((ev) => ev.u >= snap.lastUpdateId);
  if (remaining.length === 0) {
    return {
      actions: [{ kind: "applySnapshot", ...snapLevels, updateId: snap.lastUpdateId }],
      newState: { updateId: snap.lastUpdateId, mode: "bridging" },
    };
  }

  const first = remaining[0];
  if (first && !(first.U <= snap.lastUpdateId && first.u >= snap.lastUpdateId)) {
    return { actions: [{ kind: "resync" }], newState: state };
  }

  const actions: FuturesSyncAction[] = [
    { kind: "applySnapshot", ...snapLevels, updateId: snap.lastUpdateId },
  ];
  let updateId = first!.u;
  actions.push({ kind: "applyUpdates", updates: toUpdates(first!), updateId });
  for (let i = 1; i < remaining.length; i++) {
    const ev = remaining[i]!;
    if (ev.u <= updateId) continue;
    if (ev.pu !== updateId) {
      return { actions: [{ kind: "resync" }], newState: state };
    }
    actions.push({ kind: "applyUpdates", updates: toUpdates(ev), updateId: ev.u });
    updateId = ev.u;
  }
  return { actions, newState: { updateId, mode: "live" } };
}

export function reconcileFuturesEvent(
  state: FuturesSyncState,
  ev: FuturesDepthEvent,
): ReconcileEventResult {
  if (state.mode === "bridging") {
    if (ev.u < state.updateId) {
      return { action: { kind: "skip" }, newState: state };
    }
    if (ev.U > state.updateId) {
      return { action: { kind: "resync" }, newState: state };
    }
    return {
      action: { kind: "applyUpdates", updates: toUpdates(ev), updateId: ev.u },
      newState: { updateId: ev.u, mode: "live" },
    };
  }
  if (ev.u <= state.updateId) {
    return { action: { kind: "skip" }, newState: state };
  }
  if (ev.pu !== state.updateId) {
    return { action: { kind: "resync" }, newState: state };
  }
  return {
    action: { kind: "applyUpdates", updates: toUpdates(ev), updateId: ev.u },
    newState: { updateId: ev.u, mode: "live" },
  };
}
