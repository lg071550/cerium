// Venue registry: fixed venue order (indices are a wire-protocol contract
// with the C++ side — do not reorder) and per-venue native instrument maps
// for the canonical symbols. make() returns null when a venue does not
// support the requested symbol.

import type { AdapterDeps, VenueAdapter } from "./venues/types";
import { BinancePerpAdapter } from "./venues/binancePerp";
import { BinanceAdapter } from "./venues/binance";
import { BybitBaseAdapter } from "./venues/bybit";
import { OkxBookAdapter } from "./venues/okx";
import { BitgetBookAdapter } from "./venues/bitget";
import { CoinbaseL2Adapter } from "./venues/coinbase";
import { KrakenAdapter } from "./venues/kraken";
import { GateBaseAdapter } from "./venues/gate";
import { HyperliquidAdapter } from "./venues/hyperliquid";
import { BitstampAdapter } from "./venues/bitstamp";
import { CryptocomBaseAdapter } from "./venues/cryptocom";
import { BitfinexBookAdapter } from "./venues/bitfinex";
import { DeribitAdapter } from "./venues/deribit";
import { KrakenPerpAdapter } from "./venues/krakenPerp";
import { MexcPerpAdapter } from "./venues/mexcPerp";
import { AsterAdapter } from "./venues/aster";
import { LighterAdapter } from "./venues/lighter";
import { DydxAdapter } from "./venues/dydx";
import { ExtendedAdapter } from "./venues/extended";

export const SYMBOLS = ["ETH", "BTC", "SOL"] as const;

export interface VenueDef {
  index: number;
  id: string;
  label: string;
  short: string;
  make: (deps: AdapterDeps, sym: string) => VenueAdapter | null;
}

// Canonical symbol → venue-native instrument string.
const USDT: Record<string, string> = { ETH: "ETHUSDT", BTC: "BTCUSDT", SOL: "SOLUSDT" };
const OKX_SPOT: Record<string, string> = { ETH: "ETH-USDT", BTC: "BTC-USDT", SOL: "SOL-USDT" };
const OKX_PERP_CTVAL: Record<string, number> = { ETH: 0.1, BTC: 0.01, SOL: 1 };
const GATE_SPOT: Record<string, string> = { ETH: "ETH_USDT", BTC: "BTC_USDT", SOL: "SOL_USDT" };
const COINBASE_SPOT: Record<string, string> = { ETH: "ETH-USD", BTC: "BTC-USD", SOL: "SOL-USD" };
const COINBASE_PERP: Record<string, string> = {
  ETH: "ETH-PERP-INTX",
  BTC: "BTC-PERP-INTX",
  SOL: "SOL-PERP-INTX",
};
// Kraken v2 rejects "XBT/USD" ("Currency pair not supported"); BTC/USD works.
const KRAKEN_SPOT: Record<string, string> = { ETH: "ETH/USD", BTC: "BTC/USD", SOL: "SOL/USD" };
const KRAKEN_PERP: Record<string, string> = {
  ETH: "PI_ETHUSD",
  BTC: "PI_XBTUSD",
  SOL: "PI_SOLUSD",
};
const BITSTAMP_SPOT: Record<string, string> = { ETH: "ethusd", BTC: "btcusd", SOL: "solusd" };
const CRYPTOCOM_SPOT: Record<string, string> = { ETH: "ETH_USD", BTC: "BTC_USD", SOL: "SOL_USD" };
const CRYPTOCOM_PERP: Record<string, string> = {
  ETH: "ETHUSD-PERP",
  BTC: "BTCUSD-PERP",
  SOL: "SOLUSD-PERP",
};
const BITFINEX_SPOT: Record<string, string> = { ETH: "tETHUSD", BTC: "tBTCUSD", SOL: "tSOLUSD" };
const BITFINEX_PERP: Record<string, string> = {
  ETH: "tETHF0:USTF0",
  BTC: "tBTCF0:USTF0",
  SOL: "tSOLF0:USTF0",
};
const DERIBIT_PERP: Record<string, string> = {
  ETH: "ETH-PERPETUAL",
  BTC: "BTC-PERPETUAL",
  SOL: "SOL-PERPETUAL",
};
const USD_PAIR: Record<string, string> = { ETH: "ETH-USD", BTC: "BTC-USD", SOL: "SOL-USD" }; // dydx + extended
const LIGHTER_MARKET: Record<string, number> = { ETH: 0, BTC: 1, SOL: 2 };

function inst(table: Record<string, string>, sym: string): string | null {
  return table[sym] ?? null;
}

const BYBIT_SPOT_WS = "wss://stream.bybit.com/v5/public/spot";
const BYBIT_LINEAR_WS = "wss://stream.bybit.com/v5/public/linear";
const GATE_SPOT_WS = "wss://api.gateio.ws/ws/v4/";
const GATE_PERP_WS = "wss://fx-ws.gateio.ws/v4/ws/usdt";
const GATE_PERP_QUANTO = 0.01;

export const VENUES: VenueDef[] = [
  {
    index: 0,
    id: "binance-perp",
    label: "Binance Perp",
    short: "BN-P",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s ? new BinancePerpAdapter(deps, s) : null;
    },
  },
  {
    index: 1,
    id: "binance",
    label: "Binance",
    short: "BN",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s ? new BinanceAdapter(deps, s) : null;
    },
  },
  {
    index: 2,
    id: "bybit-perp",
    label: "Bybit Perp",
    short: "BB-P",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s
        ? new BybitBaseAdapter(deps, {
            id: "bybit-perp",
            symbol: `${s}-PERP`,
            wsUrl: BYBIT_LINEAR_WS,
            topic: `orderbook.1000.${s}`,
            tradeTopic: `publicTrade.${s}`,
          })
        : null;
    },
  },
  {
    index: 3,
    id: "bybit",
    label: "Bybit",
    short: "BB",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s
        ? new BybitBaseAdapter(deps, {
            id: "bybit",
            symbol: s,
            wsUrl: BYBIT_SPOT_WS,
            topic: `orderbook.1000.${s}`,
            tradeTopic: `publicTrade.${s}`,
          })
        : null;
    },
  },
  {
    index: 4,
    id: "okx-perp",
    label: "OKX Perp",
    short: "OK-P",
    make: (deps, sym) => {
      const s = inst(OKX_SPOT, sym);
      return s
        ? new OkxBookAdapter(deps, {
            id: "okx-perp",
            instId: `${s}-SWAP`,
            ctVal: OKX_PERP_CTVAL[sym] ?? 1,
          })
        : null;
    },
  },
  {
    index: 5,
    id: "okx",
    label: "OKX",
    short: "OK",
    make: (deps, sym) => {
      const s = inst(OKX_SPOT, sym);
      return s ? new OkxBookAdapter(deps, { id: "okx", instId: s, ctVal: 1 }) : null;
    },
  },
  {
    index: 6,
    id: "bitget-perp",
    label: "Bitget Perp",
    short: "BG-P",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s
        ? new BitgetBookAdapter(deps, {
            id: "bitget-perp",
            symbol: `${s}-PERP`,
            instType: "USDT-FUTURES",
            instId: s,
          })
        : null;
    },
  },
  {
    index: 7,
    id: "bitget",
    label: "Bitget",
    short: "BG",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s
        ? new BitgetBookAdapter(deps, { id: "bitget", symbol: s, instType: "SPOT" })
        : null;
    },
  },
  {
    index: 8,
    id: "coinbase",
    label: "Coinbase",
    short: "CB",
    make: (deps, sym) => {
      const s = inst(COINBASE_SPOT, sym);
      return s ? new CoinbaseL2Adapter(deps, { id: "coinbase", symbol: s }) : null;
    },
  },
  {
    index: 9,
    id: "kraken",
    label: "Kraken",
    short: "KR",
    make: (deps, sym) => {
      const s = inst(KRAKEN_SPOT, sym);
      return s ? new KrakenAdapter(deps, s) : null;
    },
  },
  {
    index: 10,
    id: "gate-perp",
    label: "Gate Perp",
    short: "GT-P",
    make: (deps, sym) => {
      // ETH-only: quanto multipliers for other symbols are unverified.
      if (sym !== "ETH") return null;
      return new GateBaseAdapter(deps, {
        id: "gate-perp",
        symbol: "ETH_USDT-PERP",
        wsUrl: GATE_PERP_WS,
        channel: "futures.obu",
        topic: "ob.ETH_USDT.400",
        sizeMultiplier: GATE_PERP_QUANTO,
        tradesChannel: "futures.trades",
        tradesPayload: ["ETH_USDT"],
      });
    },
  },
  {
    index: 11,
    id: "gate",
    label: "Gate",
    short: "GT",
    make: (deps, sym) => {
      const s = inst(GATE_SPOT, sym);
      return s
        ? new GateBaseAdapter(deps, {
            id: "gate",
            symbol: s,
            wsUrl: GATE_SPOT_WS,
            channel: "spot.obu",
            topic: `ob.${s}.400`,
            sizeMultiplier: 1,
            tradesChannel: "spot.trades",
            tradesPayload: [s],
          })
        : null;
    },
  },
  {
    index: 12,
    id: "hyperliquid",
    label: "Hyperliquid",
    short: "HL",
    make: (deps, sym) => new HyperliquidAdapter(deps, sym),
  },
  {
    index: 13,
    id: "bitstamp",
    label: "Bitstamp",
    short: "BS",
    make: (deps, sym) => {
      const s = inst(BITSTAMP_SPOT, sym);
      return s ? new BitstampAdapter(deps, s) : null;
    },
  },
  {
    index: 14,
    id: "cryptocom",
    label: "Crypto.com",
    short: "CRO",
    make: (deps, sym) => {
      const s = inst(CRYPTOCOM_SPOT, sym);
      return s
        ? new CryptocomBaseAdapter(deps, { id: "cryptocom", symbol: s, channel: `book.${s}.150` })
        : null;
    },
  },
  {
    index: 15,
    id: "cryptocom-perp",
    label: "CRO Perp",
    short: "CRO-P",
    make: (deps, sym) => {
      const s = inst(CRYPTOCOM_PERP, sym);
      return s
        ? new CryptocomBaseAdapter(deps, {
            id: "cryptocom-perp",
            symbol: s,
            channel: `book.${s}.150`,
          })
        : null;
    },
  },
  {
    index: 16,
    id: "bitfinex",
    label: "Bitfinex",
    short: "BFX",
    make: (deps, sym) => {
      const s = inst(BITFINEX_SPOT, sym);
      return s ? new BitfinexBookAdapter(deps, { id: "bitfinex", symbol: s }) : null;
    },
  },
  {
    index: 17,
    id: "bitfinex-perp",
    label: "BFX Perp",
    short: "BFX-P",
    make: (deps, sym) => {
      const s = inst(BITFINEX_PERP, sym);
      return s ? new BitfinexBookAdapter(deps, { id: "bitfinex-perp", symbol: s }) : null;
    },
  },
  {
    index: 18,
    id: "deribit",
    label: "Deribit",
    short: "DRB",
    make: (deps, sym) => {
      const s = inst(DERIBIT_PERP, sym);
      return s ? new DeribitAdapter(deps, s) : null;
    },
  },
  {
    index: 19,
    id: "kraken-perp",
    label: "Kraken Perp",
    short: "KR-P",
    make: (deps, sym) => {
      const s = inst(KRAKEN_PERP, sym);
      return s ? new KrakenPerpAdapter(deps, s) : null;
    },
  },
  {
    index: 20,
    id: "mexc-perp",
    label: "MEXC Perp",
    short: "MX-P",
    // ETH-only: contract multipliers for other symbols are unverified.
    make: (deps, sym) => (sym === "ETH" ? new MexcPerpAdapter(deps) : null),
  },
  {
    index: 21,
    id: "coinbase-perp",
    label: "CB Perp",
    short: "CB-P",
    make: (deps, sym) => {
      const s = inst(COINBASE_PERP, sym);
      return s ? new CoinbaseL2Adapter(deps, { id: "coinbase-perp", symbol: s }) : null;
    },
  },
  {
    index: 22,
    id: "coinbase-us-perp",
    label: "CB US Perp",
    short: "CBUS-P",
    // ETH-only: single dated contract (no BTC/SOL equivalent listed).
    make: (deps, sym) =>
      sym === "ETH"
        ? new CoinbaseL2Adapter(deps, {
            id: "coinbase-us-perp",
            symbol: "ETP-20DEC30-CDE",
            sizeMultiplier: 0.1,
          })
        : null,
  },
  {
    index: 23,
    id: "aster",
    label: "Aster",
    short: "AST",
    make: (deps, sym) => {
      const s = inst(USDT, sym);
      return s ? new AsterAdapter(deps, s) : null;
    },
  },
  {
    index: 24,
    id: "lighter",
    label: "Lighter",
    short: "LTR",
    make: (deps, sym) => {
      const market = LIGHTER_MARKET[sym];
      return market === undefined ? null : new LighterAdapter(deps, market, sym);
    },
  },
  {
    index: 25,
    id: "dydx",
    label: "dYdX",
    short: "DYX",
    make: (deps, sym) => {
      const s = inst(USD_PAIR, sym);
      return s ? new DydxAdapter(deps, s) : null;
    },
  },
  {
    index: 26,
    id: "extended",
    label: "Extended",
    short: "EXT",
    make: (deps, sym) => {
      const s = inst(USD_PAIR, sym);
      return s ? new ExtendedAdapter(deps, s) : null;
    },
  },
];
