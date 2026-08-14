import type { TradePrint } from "../types";
import { GateBaseAdapter, parseGateObu, parseGateTrades, type ParsedGate } from "./gate";
import type { AdapterDeps } from "./types";

const QUANTO_MULTIPLIER = 0.01;

export function parseGatePerp(msg: unknown): ParsedGate {
  return parseGateObu(msg, QUANTO_MULTIPLIER);
}

export function parseGatePerpTrades(msg: unknown): TradePrint[] | null {
  return parseGateTrades(msg, QUANTO_MULTIPLIER);
}

export class GatePerpAdapter extends GateBaseAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "gate-perp",
      symbol: "ETH_USDT-PERP",
      wsUrl: "wss://fx-ws.gateio.ws/v4/ws/usdt",
      channel: "futures.obu",
      topic: "ob.ETH_USDT.400",
      sizeMultiplier: QUANTO_MULTIPLIER,
      tradesChannel: "futures.trades",
      tradesPayload: ["ETH_USDT"],
    });
  }
}
