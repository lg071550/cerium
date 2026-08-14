import type { AdapterDeps } from "./types";
import { CoinbaseL2Adapter } from "./coinbase";

export class CoinbasePerpAdapter extends CoinbaseL2Adapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "coinbase-perp", symbol: "ETH-PERP-INTX" });
  }
}
