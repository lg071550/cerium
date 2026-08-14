import { CryptocomBaseAdapter } from "./cryptocom";
import type { AdapterDeps } from "./types";

export class CryptocomPerpAdapter extends CryptocomBaseAdapter {
  constructor(deps: AdapterDeps) {
    super(deps, {
      id: "cryptocom-perp",
      symbol: "ETHUSD-PERP",
      channel: "book.ETHUSD-PERP.150",
    });
  }
}
