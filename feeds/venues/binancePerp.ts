// Vendored from aggbook (src/venues/binancePerp.ts) — Binance USDT-M perp
// depth + trades, unchanged except import paths.

import { Reconnect } from "./reconnect";
import type { AdapterDeps, VenueAdapter } from "./types";
import { AggTradeSocket } from "./binanceTrades";
import {
  parseFuturesDepthMessage,
  parseSnapshotResponse,
  reconcileFuturesEvent,
  reconcileFuturesSnapshot,
  type FuturesDepthEvent,
  type FuturesSnapshot,
  type FuturesSyncState,
} from "./binanceSync";

export class BinancePerpAdapter implements VenueAdapter {
  readonly id = "binance-perp";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly wsUrl: string;
  private readonly restUrl: string;
  private ws: WebSocket | null = null;
  private state: FuturesSyncState = { updateId: 0, mode: "buffering" };
  private buffer: FuturesDepthEvent[] = [];
  private abortController: AbortController | null = null;
  private readonly conn: Reconnect;
  private readonly tape: AggTradeSocket;

  constructor(deps: AdapterDeps, inst = "ETHUSDT") {
    this.deps = deps;
    this.symbol = `${inst}-PERP`;
    const lower = inst.toLowerCase();
    this.wsUrl = `wss://fstream.binance.com/ws/${lower}@depth@100ms`;
    this.restUrl = `https://fapi.binance.com/fapi/v1/depth?symbol=${inst}&limit=1000`;
    this.conn = new Reconnect(
      deps.setState,
      () => this.open(),
      () => this.teardown(),
    );
    this.tape = new AggTradeSocket(`wss://fstream.binance.com/ws/${lower}@trade`, deps.onTrade);
  }

  start(): void {
    this.conn.start();
    this.tape.start();
  }

  stop(): void {
    this.conn.stop();
    this.tape.stop();
    this.deps.book.clear();
  }

  private open(): void {
    this.deps.setState("connecting");
    this.state = { updateId: 0, mode: "buffering" };
    this.buffer = [];
    const ws = new WebSocket(this.wsUrl);
    this.ws = ws;
    ws.onopen = () => {
      this.deps.setState("syncing");
      void this.fetchSnapshot();
    };
    ws.onmessage = (e) => this.handle(e);
    ws.onclose = () => this.resync("closed");
    ws.onerror = () => this.resync("socket error");
  }

  private teardown(): void {
    this.abortController?.abort();
    this.abortController = null;
    if (this.ws) {
      this.ws.onopen = null;
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
    const ev = parseFuturesDepthMessage(raw);
    if (!ev) return;

    if (this.state.mode === "buffering") {
      if(this.buffer.length>=2000) {this.resync("snapshot buffer overflow"); return;}
      this.buffer.push(ev);
      return;
    }

    const { action, newState } = reconcileFuturesEvent(this.state, ev);
    switch (action.kind) {
      case "applyUpdates": {
        this.deps.book.applyUpdates(action.updates);
        const becameLive = this.state.mode !== "live" && newState.mode === "live";
        this.state = newState;
        if (becameLive) {
          this.conn.connected();
          this.deps.setState("live");
        }
        break;
      }
      case "skip":
        break;
      case "resync":
        this.resync(
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
      const res = await fetch(this.restUrl, { signal: AbortSignal.any([abort.signal, AbortSignal.timeout(15000)]) });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = parseSnapshotResponse(await res.json());
      if (!data) throw new Error("malformed snapshot");
      snap = data;
    } catch {
      if (abort.signal.aborted || !this.conn.isRunning) return;
      this.resync("snapshot fetch failed");
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
          this.resync("snapshot stale or gappy — refetching");
          return;
        case "skip":
          break;
      }
    }

    this.state = newState;
    this.buffer = [];
    if (newState.mode === "live") {
      this.conn.connected();
      this.deps.setState("live");
    }
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
