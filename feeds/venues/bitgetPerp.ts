import type { AdapterDeps } from "./types";
import { BitgetBookAdapter } from "./bitget";

export class BitgetPerpAdapter extends BitgetBookAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "bitget-perp",
      symbol: "ETHUSDT-PERP",
      instType: "USDT-FUTURES",
      instId: "ETHUSDT",
    });
  }
}
