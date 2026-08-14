import type { AdapterDeps } from "./types";
import { OkxBookAdapter } from "./okx";

export class OkxPerpAdapter extends OkxBookAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, { id: "okx-perp", instId: "ETH-USDT-SWAP", ctVal: 0.1 });
  }
}
