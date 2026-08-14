import type { AdapterDeps } from "./types";
import { BitfinexBookAdapter } from "./bitfinex";

export class BitfinexPerpAdapter extends BitfinexBookAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "bitfinex-perp", symbol: "tETHF0:USTF0" });
  }
}
