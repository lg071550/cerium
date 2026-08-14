import { Reconnect } from "./reconnect";
import type { AdapterDeps, VenueAdapter } from "./types";
import { AggTradeSocket } from "./binanceTrades";
import {
  parseDepthMessage,
  parseSnapshotResponse,
  reconcileEvent,
  reconcileSnapshot,
  type BinanceSnapshot,
  type BinanceSyncState,
  type DepthEvent,
} from "./binanceSync";

export class BinanceAdapter implements VenueAdapter {
  readonly id = "binance";
  readonly symbol: string;

  private readonly deps: AdapterDeps;
  private readonly wsUrl: string;
  private readonly restUrl: string;
  private ws: WebSocket | null = null;
  private state: BinanceSyncState = { updateId: 0, mode: "buffering" };
  private buffer: DepthEvent[] = [];
  private abortController: AbortController | null = null;
  private readonly conn: Reconnect;
  private readonly tape: AggTradeSocket;

  constructor(deps: AdapterDeps, inst = "ETHUSDT") {
    this.deps = deps;
    this.symbol = inst;
    const lower = inst.toLowerCase();
    this.wsUrl = `wss://stream.binance.com:9443/ws/${lower}@depth@100ms`;
    this.restUrl = `https://api.binance.com/api/v3/depth?symbol=${inst}&limit=5000`;
    this.conn = new Reconnect(
      deps.setState,
      () => this.open(),
      () => this.teardown(),
    );
    // Spot uses @trade (not @aggTrade): aggTrade is silent from some networks.
    this.tape = new AggTradeSocket(
      `wss://stream.binance.com:9443/ws/${lower}@trade`,
      deps.onTrade,
    );
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
      this.conn.connected();
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
    const ev = parseDepthMessage(raw);
    if (!ev) return;

    if (this.state.mode === "buffering") {

      this.buffer.push(ev);
      return;
    }

    const { action, newState } = reconcileEvent(this.state, ev);
    switch (action.kind) {
      case "applyUpdates":
        this.deps.book.applyUpdates(action.updates);
        this.state = newState;
        break;
      case "skip":
        break;
      case "resync":
        this.resync(`diff gap: expected U <= ${this.state.updateId + 1}, got ${ev.U}`);
        break;
    }
  }

  private async fetchSnapshot(): Promise<void> {
    if (!this.conn.isRunning) return;
    this.abortController?.abort();
    const abort = new AbortController();
    this.abortController = abort;

    let snap: BinanceSnapshot;
    try {
      const res = await fetch(this.restUrl, { signal: abort.signal });
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

    const { actions, newState } = reconcileSnapshot(this.state, this.buffer, snap);
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
    this.deps.setState("live");
  }

  private resync(detail: string): void {
    this.conn.dropped(detail);
  }
}
