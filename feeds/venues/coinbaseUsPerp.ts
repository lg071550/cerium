import type { AdapterDeps } from "./types";
import { CoinbaseL2Adapter } from "./coinbase";

export class CoinbaseUsPerpAdapter extends CoinbaseL2Adapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "coinbase-us-perp", symbol: "ETP-20DEC30-CDE", sizeMultiplier: 0.1 });
  }
}
