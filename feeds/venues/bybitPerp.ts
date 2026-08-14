import { BybitBaseAdapter } from "./bybit";
import type { AdapterDeps } from "./types";

export class BybitPerpAdapter extends BybitBaseAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "bybit-perp",
      symbol: "ETHUSDT-PERP",
      wsUrl: "wss://stream.bybit.com/v5/public/linear",
      topic: "orderbook.1000.ETHUSDT",
      tradeTopic: "publicTrade.ETHUSDT",
    });
  }
}
