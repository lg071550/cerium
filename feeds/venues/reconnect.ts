// Vendored from aggbook (src/venues/reconnect.ts) — exponential backoff
// reconnect helper. Only the import path changed.

import type { FeedState } from "../types";

export class Reconnect {
  private timer: ReturnType<typeof setTimeout> | null = null;
  private attempt = 0;
  private running = false;

  constructor(
    private readonly setState: (state: FeedState, detail?: string) => void,
    private readonly open: () => void,
    private readonly teardown: () => void,
  ) {}

  start(): void {
    if (this.running) return;
    this.running = true;
    this.tryOpen();
  }

  stop(): void {
    this.running = false;
    this.attempt = 0;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.teardown();
  }

  connected(): void {
    this.attempt = 0;
  }

  dropped(detail?: string): void {
    if (!this.running || this.timer !== null) return;
    this.setState("reconnecting", detail);
    this.teardown();
    const delay = Math.min(1000 * 2 ** this.attempt, 30000);
    this.attempt += 1;
    this.timer = setTimeout(() => {
      this.timer = null;
      if (this.running) this.tryOpen();
    }, delay);
  }

  get isRunning(): boolean {
    return this.running;
  }

  private tryOpen(): void {
    try {
      this.open();
    } catch (error) {
      const detail =
        error instanceof Error ? `connect failed: ${error.message}` : "connect failed";
      this.dropped(detail);
    }
  }
}
