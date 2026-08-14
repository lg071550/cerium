import { Reconnect } from "./reconnect";
import type { AdapterDeps, VenueAdapter } from "./types";
import { isRecord } from "./types";
import { parseAggTrade } from "./binanceTrades";
import type { TradePrint } from "../types";
import {
  parseFuturesDepthMessage,
  parseSnapshotResponse,
  reconcileFuturesEvent,
  reconcileFuturesSnapshot,
  type FuturesDepthEvent,
  type FuturesSnapshot,
  type FuturesSyncState,
} from "./binanceSync";

const FALLBACK_INTERVAL_MS = 1000;

interface RestTrade {
  id: number;
  print: TradePrint;
}

function parseRestTrades(raw: unknown): RestTrade[] | null {
  if (!Array.isArray(raw)) return null;
  const out: RestTrade[] = [];
  for (const item of raw) {
    if (
      !isRecord(item) ||
      typeof item.id !== "number" ||
      !Number.isSafeInteger(item.id) ||
      typeof item.price !== "string" ||
      typeof item.qty !== "string" ||
      typeof item.time !== "number" ||
      !Number.isFinite(item.time) ||
      typeof item.isBuyerMaker !== "boolean"
    ) {
      return null;
    }
    const price = Number(item.price);
    const size = Number(item.qty);
    if (!Number.isFinite(price) || price <= 0 || !Number.isFinite(size) || size <= 0) {
      return null;
    }
    out.push({
      id: item.id,
      print: {
        price,
        size,
        side: item.isBuyerMaker ? "sell" : "buy",
        ts: item.time,
      },
    });
  }
  return out;
}

export class AsterAdapter implements VenueAdapter {
  readonly id = "aster";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly wsUrl: string;
  private readonly restUrl: string;
  private readonly fallbackDepthUrl: string;
  private readonly fallbackTradesUrl: string;
  private ws: WebSocket | null = null;
  private state: FuturesSyncState = { updateId: 0, mode: "buffering" };
  private buffer: FuturesDepthEvent[] = [];
  private abortController: AbortController | null = null;
  private socketErrorTimer: ReturnType<typeof setTimeout> | null = null;
  private snapshotRetryTimer: ReturnType<typeof setTimeout> | null = null;
  private snapshotAttempt = 0;
  private fallbackTimer: ReturnType<typeof setTimeout> | null = null;
  private fallbackActive = false;
  private fallbackLive = false;
  private lastFallbackTradeId: number | null = null;
  private readonly conn: Reconnect;

  constructor(deps: AdapterDeps, inst = "ETHUSDT") {
    this.deps = deps;
    this.symbol = inst;
    const lower = inst.toLowerCase();
    this.wsUrl = `wss://fstream.asterdex.com/stream?streams=${lower}@depth@100ms/${lower}@aggTrade`;
    this.restUrl = `https://fapi.asterdex.com/fapi/v3/depth?symbol=${inst}&limit=1000`;
    this.fallbackDepthUrl = `https://fapi.asterdex.com/fapi/v3/depth?symbol=${inst}&limit=500`;
    this.fallbackTradesUrl = `https://fapi.asterdex.com/fapi/v3/trades?symbol=${inst}&limit=100`;
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
    this.state = { updateId: 0, mode: "buffering" };
    this.buffer = [];
    this.snapshotAttempt = 0;
    const ws = new WebSocket(this.wsUrl);
    this.ws = ws;
    ws.onopen = () => {
      this.deps.setState("syncing");

      void this.fetchSnapshot();
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = (event) => {
      this.clearSocketErrorTimer();
      if (event.code === 1006) {
        this.startRestFallback("WebSocket closed 1006");
        return;
      }
      const reason = event.reason.trim();
      this.resync(`closed ${event.code}${reason ? `: ${reason}` : ""}`);
    };
    ws.onerror = () => {
      this.clearSocketErrorTimer();
      this.socketErrorTimer = setTimeout(() => {
        this.socketErrorTimer = null;
        if (this.ws === ws) this.startRestFallback("WebSocket error without close event");
      }, 1500);
    };
  }

  private teardown(): void {
    this.clearSocketErrorTimer();
    if (this.snapshotRetryTimer) {
      clearTimeout(this.snapshotRetryTimer);
      this.snapshotRetryTimer = null;
    }
    if (this.fallbackTimer) {
      clearTimeout(this.fallbackTimer);
      this.fallbackTimer = null;
    }
    this.fallbackActive = false;
    this.fallbackLive = false;
    this.lastFallbackTradeId = null;
    this.abortController?.abort();
    this.abortController = null;
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }
    this.deps.book.clear();
    this.state = { updateId: 0, mode: "buffering" };
    this.buffer = [];
  }

  private handle(event: MessageEvent): void {
    let raw: unknown;
    try {
      raw = JSON.parse(event.data as string);
    } catch {
      return;
    }
    if (isRecord(raw) && isRecord(raw.data)) raw = raw.data;

    const prints = parseAggTrade(raw);
    if (prints) {
      this.deps.onTrade?.(prints);
      return;
    }

    const ev = parseFuturesDepthMessage(raw);
    if (!ev) return;

    if (this.state.mode === "buffering") {
      this.buffer.push(ev);
      return;
    }

    const { action, newState } = reconcileFuturesEvent(this.state, ev);
    switch (action.kind) {
      case "applyUpdates":
        this.deps.book.applyUpdates(action.updates);
        const becameLive = this.state.mode !== "live" && newState.mode === "live";
        this.state = newState;
        if (becameLive) {
          this.conn.connected();
          this.deps.setState("live");
        }
        break;
      case "skip":
        break;
      case "resync":
        this.restartSnapshot(
          this.state.mode === "bridging"
            ? `snapshot bridge gap: expected U <= ${this.state.updateId} <= u, got ${ev.U}-${ev.u}`
            : `diff gap: expected pu ${this.state.updateId}, got ${ev.pu}`,
        );
        break;
    }
  }

  private async fetchSnapshot(): Promise<void> {
    if (!this.conn.isRunning) return;
    this.abortController?.abort();
    const abort = new AbortController();
    this.abortController = abort;

    let snap: FuturesSnapshot;
    try {
      const res = await fetch(this.restUrl, { signal: abort.signal });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = parseSnapshotResponse(await res.json());
      if (!data) throw new Error("malformed snapshot");
      snap = data;
    } catch (error) {
      if (abort.signal.aborted || !this.conn.isRunning) return;
      const detail = error instanceof Error ? error.message : "unknown error";
      this.scheduleSnapshotRetry(`snapshot fetch failed: ${detail}`);
      return;
    }

    if (abort.signal.aborted || !this.conn.isRunning) return;

    const { actions, newState } = reconcileFuturesSnapshot(this.state, this.buffer, snap);
    for (const action of actions) {
      switch (action.kind) {
        case "applySnapshot":
          this.deps.book.applySnapshot(action.bids, action.asks);
          break;
        case "applyUpdates":
          this.deps.book.applyUpdates(action.updates);
          break;
        case "resync":
          this.restartSnapshot("snapshot stale or gappy");
          return;
        case "skip":
          break;
      }
    }

    this.state = newState;
    this.buffer = [];
    this.snapshotAttempt = 0;
    if (newState.mode === "live") {
      this.conn.connected();
      this.deps.setState("live");
    }
  }

  private restartSnapshot(detail: string): void {
    if (!this.conn.isRunning || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      this.resync(detail);
      return;
    }
    this.abortController?.abort();
    this.abortController = null;
    if (this.snapshotRetryTimer) {
      clearTimeout(this.snapshotRetryTimer);
      this.snapshotRetryTimer = null;
    }
    this.state = { updateId: 0, mode: "buffering" };
    this.buffer = [];
    this.deps.setState("syncing", detail);
    void this.fetchSnapshot();
  }

  private scheduleSnapshotRetry(detail: string): void {
    if (!this.conn.isRunning || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      this.resync(detail);
      return;
    }
    this.state = { updateId: 0, mode: "buffering" };
    this.buffer = [];
    this.deps.setState("syncing", detail);
    const delay = Math.min(1000 * 2 ** this.snapshotAttempt, 30000);
    this.snapshotAttempt++;
    if (this.snapshotRetryTimer) clearTimeout(this.snapshotRetryTimer);
    this.snapshotRetryTimer = setTimeout(() => {
      this.snapshotRetryTimer = null;
      if (this.conn.isRunning && this.ws?.readyState === WebSocket.OPEN) {
        void this.fetchSnapshot();
      }
    }, delay);
  }

  private clearSocketErrorTimer(): void {
    if (!this.socketErrorTimer) return;
    clearTimeout(this.socketErrorTimer);
    this.socketErrorTimer = null;
  }

  private startRestFallback(detail: string): void {
    if (!this.conn.isRunning || this.fallbackActive) return;
    this.clearSocketErrorTimer();
    if (this.snapshotRetryTimer) {
      clearTimeout(this.snapshotRetryTimer);
      this.snapshotRetryTimer = null;
    }
    this.abortController?.abort();
    this.abortController = null;
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      if (this.ws.readyState === WebSocket.OPEN) this.ws.close();
      this.ws = null;
    }
    this.state = { updateId: 0, mode: "buffering" };
    this.buffer = [];
    this.fallbackActive = true;
    this.fallbackLive = false;
    this.lastFallbackTradeId = null;
    this.deps.setState("syncing", `${detail}; switching to REST fallback`);
    void this.pollRestFallback();
  }

  private async pollRestFallback(): Promise<void> {
    if (!this.conn.isRunning || !this.fallbackActive) return;
    const abort = new AbortController();
    this.abortController = abort;
    try {
      const [depthResponse, tradesResponse] = await Promise.all([
        fetch(this.fallbackDepthUrl, { signal: abort.signal }),
        fetch(this.fallbackTradesUrl, { signal: abort.signal }),
      ]);
      if (!depthResponse.ok) throw new Error(`depth HTTP ${depthResponse.status}`);
      if (!tradesResponse.ok) throw new Error(`trades HTTP ${tradesResponse.status}`);
      const snapshot = parseSnapshotResponse(await depthResponse.json());
      if (!snapshot) throw new Error("malformed depth response");
      const trades = parseRestTrades(await tradesResponse.json());
      if (!trades) throw new Error("malformed trades response");
      if (abort.signal.aborted || !this.conn.isRunning || !this.fallbackActive) return;

      this.deps.book.applySnapshot(
        snapshot.bids.map(([price, size]) => ({ price: Number(price), size: Number(size) })),
        snapshot.asks.map(([price, size]) => ({ price: Number(price), size: Number(size) })),
      );

      const newestTradeId = trades.reduce<number | null>(
        (latest, trade) => (latest === null || trade.id > latest ? trade.id : latest),
        null,
      );
      if (this.lastFallbackTradeId !== null) {
        const prints = trades
          .filter((trade) => trade.id > this.lastFallbackTradeId!)
          .map((trade) => trade.print);
        if (prints.length) this.deps.onTrade?.(prints);
      }
      if (newestTradeId !== null) this.lastFallbackTradeId = newestTradeId;

      if (!this.fallbackLive) {
        this.fallbackLive = true;
        this.conn.connected();
        this.deps.setState("live", "REST fallback");
      }
    } catch (error) {
      if (abort.signal.aborted || !this.conn.isRunning || !this.fallbackActive) return;
      const detail = error instanceof Error ? error.message : "unknown error";
      this.deps.setState(
        this.fallbackLive ? "live" : "syncing",
        `REST fallback retry: ${detail}`,
      );
    } finally {
      if (this.abortController === abort) this.abortController = null;
      if (this.conn.isRunning && this.fallbackActive) {
        this.fallbackTimer = setTimeout(() => {
          this.fallbackTimer = null;
          void this.pollRestFallback();
        }, FALLBACK_INTERVAL_MS);
      }
    }
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
