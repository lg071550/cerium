// Aggregated derivatives market data for future indicators: open interest
// (base-asset contracts, summed across perp venues), a median funding rate,
// and liquidation prints.
//
// OI history is 5m stats from Binance/Bybit/OKX/Gate (carry-forward sum).
// Live OI/funding are native-timestamp samples from ticker websockets plus a
// REST fallback — not folded into the 5m/1h hist buckets, which had been
// collapsing CVD-like movement into a staircase. Funding history stays
// settled 1h/8h stamps; the live tail is the predicted/mark rate.
//
// Posted to the main thread as `{ kind: "market", sym, oi, funding, liq }`
// with transferable packed arrays:
//   oi       [tsMs, coin] × n     (5m hist, then 1s live)
//   funding  [tsMs, rate] × n     (settlements, then live predicted)
//   liq      [tsMs, price, qty, side] × n   (side 0=buy 1=sell)
// Live prints also post `{ kind: "marketLiq", start, rows, sym }` so the
// panel updates per print instead of waiting for the next OI poll.

import { SYMBOLS } from "./registry";
import { flowWalkActive, whenFlowWalkQuiet } from "./candles";

const POLL_MS = 15000; // REST fallback for venues without a ticker socket
const BINANCE_OI_MS = 2000; // no OI websocket; this is the finest public poll
const OI_STEP_MS = 5 * 60 * 1000; // exchange hist min; live is not aligned to this
const LIVE_GAP_MS = 1000; // one aggregated sample per second
const MAX_SAMPLES = 8192; // ~28 days of 5m OI hist, or ~2.3h of 1s live
const BYBIT_PING_MS = 20000;
const OKX_PING_MS = 25000;
const GATE_PING_MS = 20000;

// USDT-margined perp symbol — shared by Binance, Bitget, and Aster.
const USDT_PERP: Record<string, string> = { ETH: "ETHUSDT", BTC: "BTCUSDT", SOL: "SOLUSDT" };
const BINANCE_COINM: Record<string, string> = {
  ETH: "ETHUSD_PERP", BTC: "BTCUSD_PERP", SOL: "SOLUSD_PERP",
};
const BINANCE_COINM_USD: Record<string, number> = { ETH: 10, BTC: 100, SOL: 10 };
const BYBIT_PERP: Record<string, string> = { ETH: "ETHUSDT", BTC: "BTCUSDT", SOL: "SOLUSDT" };
const OKX_PERP: Record<string, string> = {
  ETH: "ETH-USDT-SWAP", BTC: "BTC-USDT-SWAP", SOL: "SOL-USDT-SWAP",
};
const GATE_PERP: Record<string, string> = { ETH: "ETH_USDT", BTC: "BTC_USDT", SOL: "SOL_USDT" };
const DERIBIT_PERP: Record<string, string> = {
  ETH: "ETH-PERPETUAL", BTC: "BTC-PERPETUAL", SOL: "SOL_USDC-PERPETUAL",
};
const BITFINEX_PERP: Record<string, string> = {
  ETH: "tETHF0:USTF0", BTC: "tBTCF0:USTF0", SOL: "tSOLF0:USTF0",
};
const LIGHTER_MARKET: Record<string, number> = { ETH: 0, BTC: 1, SOL: 2 };
const DYDX_PERP: Record<string, string> = { ETH: "ETH-USD", BTC: "BTC-USD", SOL: "SOL-USD" };
// Same contract multipliers as feeds/registry.ts OKX_PERP_CTVAL / GATE_PERP_QUANTO.
const OKX_CTVAL: Record<string, number> = { ETH: 0.1, BTC: 0.01, SOL: 1 };
const GATE_CTVAL = 0.01;
const HTX_PERP: Record<string, string> = { ETH: "ETH-USDT", BTC: "BTC-USDT", SOL: "SOL-USDT" };
const HTX_CTVAL: Record<string, number> = { ETH: 0.01, BTC: 0.001, SOL: 1 };
const MEXC_PERP: Record<string, string> = { ETH: "ETH_USDT", BTC: "BTC_USDT", SOL: "SOL_USDT" };
const MEXC_CTVAL: Record<string, number> = { ETH: 0.01, BTC: 0.0001, SOL: 1 };
const KRAKEN_PERP: Record<string, string> = {
  ETH: "PI_ETHUSD", BTC: "PI_XBTUSD", SOL: "PI_SOLUSD",
};
const GATE_WS = "wss://fx-ws.gateio.ws/v4/ws/usdt";
const ASTER_FAPI = "https://fapi.asterdex.com";
const HL_WS = "wss://api.hyperliquid.xyz/ws";
const HL_RPC_WS = "wss://rpc.hyperliquid.xyz/ws";
const HL_INFO = "https://api.hyperliquid.xyz/info";
const HL_PING_MS = 20000;
const HL_COIN: Record<string, number> = { BTC: 0, ETH: 1, SOL: 5 };
// HLP liquidator children + standalone protocol Liquidator vault. Book
// liquidations land on the victim; backstop fills land on these addresses
// with a `liquidation` object. No API key — userFills is public per address.
const HL_LIQUIDATORS = [
  "0x2e3d94f0562703b25c83308a05046ddaf9a8dd14",
  "0xb0a55f13d22f66e6d495ac98113841b2326e9540",
  "0x5e177e5e39c0f4e421f5865a6d8beed8d921cb70",
  "0x2ed5c4484ea3ff8b57d5f2fb152a40d9f2b68308",
  "0x63c621a33714ec48660e32f2374895c8026a3a00",
];

interface VenueMarket {
  oi: number | null; // base-asset contracts (ETH/BTC/SOL)
  funding: number | null;
}

const Snap = {
  Binance: 0,
  Bybit: 1,
  Okx: 2,
  Gate: 3,
  Bitget: 4,
  Deribit: 5,
  Dydx: 6,
  Hyperliquid: 7,
  Htx: 8,
  Aster: 9,
  Bitfinex: 10,
  Kraken: 11,
  Mexc: 12,
  Lighter: 13,
} as const;
const SNAP_N = 14;

async function fetchJson(url: string): Promise<unknown | null> {
  try {
    const res = await fetch(url, { signal: AbortSignal.timeout(8000) });
    if (!res.ok) return null;
    return await res.json();
  } catch {
    return null;
  }
}

async function postJson(url: string, body: unknown): Promise<unknown | null> {
  try {
    const res = await fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
      signal: AbortSignal.timeout(8000),
    });
    if (!res.ok) return null;
    return await res.json();
  } catch {
    return null;
  }
}

function num(v: unknown): number {
  const n = Number(v);
  return Number.isFinite(n) ? n : NaN;
}

function parseObj(raw: string): Record<string, unknown> | null {
  try {
    const v = JSON.parse(raw) as unknown;
    return v !== null && typeof v === "object" ? (v as Record<string, unknown>) : null;
  } catch {
    return null;
  }
}

async function binanceStyleMarket(host: string, s: string): Promise<VenueMarket> {
  const [oiRaw, premRaw] = await Promise.all([
    fetchJson(`${host}/fapi/v1/openInterest?symbol=${s}`),
    fetchJson(`${host}/fapi/v1/premiumIndex?symbol=${s}`),
  ]);
  const oi = oiRaw as Record<string, unknown> | null;
  const prem = premRaw as Record<string, unknown> | null;
  if (!oi || !prem) return { oi: null, funding: null };
  // USD-M `openInterest` is already in base units (ETH/BTC/SOL).
  const base = num(oi.openInterest);
  const funding = Number.isFinite(num(prem.lastFundingRate))
    ? num(prem.lastFundingRate)
    : null;
  return {
    oi: Number.isFinite(base) && base >= 0 ? base : null,
    funding,
  };
}

async function binanceMarket(sym: string): Promise<VenueMarket> {
  const s = USDT_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const m = await binanceStyleMarket("https://fapi.binance.com", s);
  // REST lastFundingRate is the 8h settlement; predicted `r` arrives on
  // markPrice@1s. Passing funding here would flatten the live series.
  return { oi: m.oi, funding: null };
}

async function asterMarket(sym: string): Promise<VenueMarket> {
  const s = USDT_PERP[sym];
  if (!s) return { oi: null, funding: null };
  return binanceStyleMarket(ASTER_FAPI, s);
}

async function bybitMarket(sym: string): Promise<VenueMarket> {
  const s = BYBIT_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://api.bybit.com/v5/market/tickers?category=linear&symbol=${s}`,
  );
  const list = (raw as { result?: { list?: unknown[] } } | null)?.result?.list;
  if (!Array.isArray(list) || list.length === 0) return { oi: null, funding: null };
  const t = list[0] as Record<string, unknown>;
  const oi = num(t.openInterest);
  const f = num(t.fundingRate);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: Number.isFinite(f) ? f : null,
  };
}

async function okxMarket(sym: string): Promise<VenueMarket> {
  const s = OKX_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const [oiRaw, fRaw] = await Promise.all([
    fetchJson(`https://www.okx.com/api/v5/public/open-interest?instId=${s}`),
    fetchJson(`https://www.okx.com/api/v5/public/funding-rate?instId=${s}`),
  ]);
  const oiData = (oiRaw as { data?: unknown[] } | null)?.data;
  const fData = (fRaw as { data?: unknown[] } | null)?.data;
  let oi: number | null = null;
  let funding: number | null = null;
  if (Array.isArray(oiData) && oiData.length) {
    const row = oiData[0] as Record<string, unknown>;
    const v = num(row.oiCcy);
    if (Number.isFinite(v) && v >= 0) oi = v;
  }
  if (Array.isArray(fData) && fData.length) {
    const f = num((fData[0] as Record<string, unknown>).fundingRate);
    if (Number.isFinite(f)) funding = f;
  }
  return { oi, funding };
}

async function gateMarket(sym: string): Promise<VenueMarket> {
  const contract = GATE_PERP[sym];
  if (!contract) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://api.gateio.ws/api/v4/futures/usdt/tickers?contract=${contract}`,
  );
  const t = Array.isArray(raw) ? raw[0] as Record<string, unknown> | undefined
    : raw && typeof raw === "object" ? raw as Record<string, unknown> : undefined;
  if (!t) return { oi: null, funding: null };
  // total_size is contracts; quanto 0.01 for ETH/BTC. position_size is not OI.
  const size = num(t.total_size);
  const oi = Number.isFinite(size) && size >= 0 ? size * GATE_CTVAL : null;
  const funding = num(t.funding_rate);
  return { oi, funding: Number.isFinite(funding) ? funding : null };
}

async function bitgetMarket(sym: string): Promise<VenueMarket> {
  const s = USDT_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://api.bitget.com/api/v2/mix/market/ticker?productType=USDT-FUTURES&symbol=${s}`,
  );
  const list = (raw as { data?: unknown[] } | null)?.data;
  if (!Array.isArray(list) || list.length === 0) return { oi: null, funding: null };
  const t = list[0] as Record<string, unknown>;
  const oi = num(t.holdingAmount ?? t.openInterest);
  const funding = num(t.fundingRate);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function deribitMarket(sym: string): Promise<VenueMarket> {
  const inst = DERIBIT_PERP[sym];
  if (!inst) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://www.deribit.com/api/v2/public/get_book_summary_by_instrument?instrument_name=${inst}`,
  );
  const data = (raw as { result?: unknown[] } | null)?.result;
  if (!Array.isArray(data)) return { oi: null, funding: null };
  for (const row of data) {
    if (!row || typeof row !== "object") continue;
    const r = row as Record<string, unknown>;
    if (r.instrument_name !== inst) continue;
    // Inverse perp OI is USD notional; convert to coin.
    const usd = num(r.open_interest);
    const mark = num(r.mark_price ?? r.mark);
    const funding = num(r.current_funding);
    const oi = Number.isFinite(usd) && usd >= 0 && mark > 0 ? (inst.includes("_USDC") ? usd : usd / mark) : null;
    return { oi, funding: Number.isFinite(funding) ? funding : null };
  }
  return { oi: null, funding: null };
}

async function dydxMarket(sym: string): Promise<VenueMarket> {
  const raw = await fetchJson(`https://indexer.dydx.trade/v4/perpetualMarkets`);
  const markets = (raw as { markets?: Record<string, unknown> } | null)?.markets;
  if (!markets || typeof markets !== "object") return { oi: null, funding: null };
  const key = `${sym}-USD`;
  const m = markets[key] as Record<string, unknown> | undefined;
  if (!m) return { oi: null, funding: null };
  const oi = num(m.openInterest);
  const funding = num(m.nextFundingRate);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function hyperliquidMarket(sym: string): Promise<VenueMarket> {
  const raw = await postJson(HL_INFO, { type: "metaAndAssetCtxs" });
  if (!Array.isArray(raw) || raw.length < 2) return { oi: null, funding: null };
  const meta = raw[0] as Record<string, unknown>;
  const ctxs = raw[1];
  if (!meta || !Array.isArray(ctxs)) return { oi: null, funding: null };
  const universe = meta.universe as Array<Record<string, unknown>> | undefined;
  if (!Array.isArray(universe)) return { oi: null, funding: null };
  let idx = -1;
  for (let i = 0; i < universe.length; ++i) {
    if (String(universe[i]?.name ?? "") === sym) { idx = i; break; }
  }
  if (idx < 0 || idx >= ctxs.length) return { oi: null, funding: null };
  const ctx = ctxs[idx] as Record<string, unknown>;
  const oi = num(ctx.openInterest);
  const funding = num(ctx.funding);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function htxMarket(sym: string): Promise<VenueMarket> {
  const code = HTX_PERP[sym];
  if (!code) return { oi: null, funding: null };
  const [oiRaw, fRaw] = await Promise.all([
    fetchJson(`https://api.hbdm.com/linear-swap-api/v1/swap_open_interest?contract_code=${code}`),
    fetchJson(`https://api.hbdm.com/linear-swap-api/v1/swap_funding_rate?contract_code=${code}`),
  ]);
  const oiRow = Array.isArray((oiRaw as { data?: unknown } | null)?.data)
    ? ((oiRaw as { data: unknown[] }).data[0] as Record<string, unknown> | undefined)
    : (oiRaw as { data?: Record<string, unknown> } | null)?.data;
  const fData = (fRaw as { data?: unknown } | null)?.data;
  const fRow = Array.isArray(fData)
    ? (fData[0] as Record<string, unknown> | undefined)
    : fData && typeof fData === "object" ? fData as Record<string, unknown> : undefined;
  const oi = num(oiRow?.amount);
  const funding = num(fRow?.funding_rate);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function bitfinexMarket(sym: string): Promise<VenueMarket> {
  const s = BITFINEX_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://api-pub.bitfinex.com/v2/status/deriv?keys=${encodeURIComponent(s)}`,
  );
  if (!Array.isArray(raw) || raw.length === 0 || !Array.isArray(raw[0]))
    return { oi: null, funding: null };
  const row = raw[0] as unknown[];
  const oi = num(row[17]) || num(row[18]);
  const funding = num(row[12]);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function krakenMarket(sym: string): Promise<VenueMarket> {
  const s = KRAKEN_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const raw = await fetchJson(`https://futures.kraken.com/derivatives/api/v3/tickers/${s}`);
  const t = (raw as { ticker?: Record<string, unknown> } | null)?.ticker;
  if (!t) return { oi: null, funding: null };
  const usd = num(t.openInterest);
  const mark = num(t.markPrice);
  const funding = num(t.fundingRatePrediction ?? t.fundingRate);
  return {
    oi: Number.isFinite(usd) && usd >= 0 && mark > 0 ? (inst.includes("_USDC") ? usd : usd / mark) : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function mexcMarket(sym: string): Promise<VenueMarket> {
  const s = MEXC_PERP[sym];
  if (!s) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://contract.mexc.com/api/v1/contract/ticker?symbol=${s}`,
  );
  const t = (raw as { data?: Record<string, unknown> } | null)?.data;
  if (!t) return { oi: null, funding: null };
  const hold = num(t.holdVol);
  const ct = MEXC_CTVAL[sym] ?? 1;
  const funding = num(t.fundingRate);
  return {
    oi: Number.isFinite(hold) && hold >= 0 ? hold * ct : null,
    funding: Number.isFinite(funding) ? funding : null,
  };
}

async function lighterMarket(sym: string): Promise<VenueMarket> {
  const market = LIGHTER_MARKET[sym];
  if (market === undefined) return { oi: null, funding: null };
  const raw = await fetchJson(
    `https://mainnet.zklighter.elliot.ai/api/v1/orderBookDetails?market_id=${market}`,
  );
  const list = (raw as { order_book_details?: unknown[] } | null)?.order_book_details;
  if (!Array.isArray(list) || list.length === 0) return { oi: null, funding: null };
  const t = list[0] as Record<string, unknown>;
  const oi = num(t.open_interest);
  return {
    oi: Number.isFinite(oi) && oi >= 0 ? oi : null,
    funding: null,
  };
}

const SOURCES: { snap: number; fn: (sym: string) => Promise<VenueMarket> }[] = [
  { snap: Snap.Binance, fn: binanceMarket },
  { snap: Snap.Bybit, fn: bybitMarket },
  { snap: Snap.Okx, fn: okxMarket },
  { snap: Snap.Gate, fn: gateMarket },
  { snap: Snap.Bitget, fn: bitgetMarket },
  { snap: Snap.Deribit, fn: deribitMarket },
  { snap: Snap.Dydx, fn: dydxMarket },
  { snap: Snap.Hyperliquid, fn: hyperliquidMarket },
  { snap: Snap.Htx, fn: htxMarket },
  { snap: Snap.Aster, fn: asterMarket },
  { snap: Snap.Bitfinex, fn: bitfinexMarket },
  { snap: Snap.Kraken, fn: krakenMarket },
  { snap: Snap.Mexc, fn: mexcMarket },
  { snap: Snap.Lighter, fn: lighterMarket },
];

let symIndex = 0;
let timer: ReturnType<typeof setInterval> | null = null;
let binanceTimer: ReturnType<typeof setInterval> | null = null;
let publishTimer: ReturnType<typeof setTimeout> | null = null;
let liqGen = 0;
const liqSockets: WebSocket[] = [];
const liqTimers: ReturnType<typeof setTimeout>[] = [];
const oiHist: number[] = [];
const oiLive: number[] = [];
const fundHist: number[] = [];
const fundLive: number[] = [];
const liqHistory: number[] = [];
const liqSeen = new Set<string>();
let liqCounter = 0; // total records ever received (monotonic, never reset by trim)
let liqBase = 0;    // global index of the first retained liqHistory row
let liqPostedCount = -1; // liqCounter at the last full-history post (-1 = force)
const snaps: VenueMarket[] = Array.from({ length: SNAP_N }, () => ({
  oi: null, funding: null,
}));

function push2(arr: number[], ts: number, v: number): void {
  arr.push(ts, v);
  if (arr.length > MAX_SAMPLES * 2) arr.splice(0, arr.length - MAX_SAMPLES * 2);
}

function alignOiTs(ts: number): number {
  return Math.floor(ts / OI_STEP_MS) * OI_STEP_MS;
}

function upsertLive(arr: number[], ts: number, v: number): void {
  if (!(ts > 0) || !Number.isFinite(v)) return;
  const n = arr.length;
  if (n >= 2 && arr[n - 2] > ts) return;
  if (n >= 2 && ts - arr[n - 2] < LIVE_GAP_MS) {
    arr[n - 2] = ts;
    arr[n - 1] = v;
    return;
  }
  push2(arr, ts, v);
}

function setSnap(i: number, oi: number | null, funding: number | null, kick = true): void {
  const s = snaps[i];
  if (oi != null && Number.isFinite(oi) && oi >= 0) s.oi = oi;
  if (funding != null && Number.isFinite(funding)) s.funding = funding;
  if (kick) schedulePublish();
}

function clearSnaps(): void {
  for (const s of snaps) { s.oi = null; s.funding = null; }
}

function packSeries(hist: number[], live: number[], alignLive = false): Float64Array {
  const liveStart = live.length >= 2 ? live[0] : Infinity;
  let hLen = 0;
  for (let i = 0; i < hist.length; i += 2) {
    if (hist[i] < liveStart) hLen += 2;
    else break;
  }
  // Hist is 4 venues; live is up to 14. Shift hist so its last point meets
  // current live — otherwise the coverage jump owns the Y scale and the 5m
  // texture collapses to a flat line (the MMTS comparison).
  let lift = 0;
  if (alignLive && hLen >= 2 && live.length >= 2) {
    lift = live[live.length - 1] - hist[hLen - 1];
  }
  const out = new Float64Array(hLen + live.length);
  for (let i = 0; i < hLen; i += 2) {
    out[i] = hist[i];
    out[i + 1] = hist[i + 1] + lift;
  }
  for (let i = 0; i < live.length; i++) out[hLen + i] = live[i];
  return out;
}

interface OiPt { ts: number; coin: number }
type OiSink = (pts: OiPt[]) => void;

function sortPts(pts: OiPt[]): OiPt[] {
  pts.sort((a, b) => a.ts - b.ts);
  const out: OiPt[] = [];
  for (const p of pts) {
    if (!p.ts || !Number.isFinite(p.coin) || p.coin <= 0) continue;
    const ts = alignOiTs(p.ts);
    if (out.length && out[out.length - 1].ts === ts) out[out.length - 1] = { ts, coin: p.coin };
    else out.push({ ts, coin: p.coin });
  }
  return out;
}

function mergeOi(parts: OiPt[][]): number[] {
  const maps = parts.map((s) => {
    const m = new Map<number, number>();
    for (const p of s) m.set(p.ts, p.coin);
    return m;
  });
  let minT = Infinity;
  let maxT = -Infinity;
  for (const m of maps) {
    for (const t of m.keys()) {
      if (t < minT) minT = t;
      if (t > maxT) maxT = t;
    }
  }
  if (!Number.isFinite(minT)) return [];
  const maxSpan = (MAX_SAMPLES - 1) * OI_STEP_MS;
  if (maxT - minT > maxSpan) minT = maxT - maxSpan;
  const last: (number | null)[] = parts.map(() => null);
  const out: number[] = [];
  for (let t = minT; t <= maxT; t += OI_STEP_MS) {
    let sum = 0;
    let n = 0;
    for (let i = 0; i < maps.length; i++) {
      const v = maps[i].get(t);
      if (v !== undefined) last[i] = v;
      if (last[i] !== null) {
        sum += last[i]!;
        n++;
      }
    }
    if (n > 0) out.push(t, sum);
  }
  const cap = MAX_SAMPLES * 2;
  return out.length > cap ? out.slice(out.length - cap) : out;
}

async function binanceOiHist(sym: string, sink?: OiSink): Promise<OiPt[]> {
  const s = USDT_PERP[sym];
  const pts: OiPt[] = [];
  if (!s) return pts;
  let endTime: number | undefined;
  for (let page = 0; page < 6; page++) {
    let url =
      `https://fapi.binance.com/futures/data/openInterestHist?symbol=${s}&period=5m&limit=500`;
    if (endTime) url += `&endTime=${endTime}`;
    const raw = await fetchJson(url);
    if (!Array.isArray(raw) || raw.length === 0) break;
    const batch: OiPt[] = [];
    let oldest = Infinity;
    for (const row of raw) {
      if (!row || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      const ts = num(r.timestamp);
      const coin = num(r.sumOpenInterest);
      if (!(ts > 0) || !(coin > 0)) continue;
      batch.push({ ts, coin });
      if (ts < oldest) oldest = ts;
    }
    if (batch.length === 0 || !Number.isFinite(oldest)) break;
    pts.push(...batch);
    sink?.(sortPts(pts));
    if (batch.length < 400) break;
    endTime = oldest - 1;
    if (flowWalkActive()) await new Promise((r) => setTimeout(r, 250));
  }
  return sortPts(pts);
}

async function bybitOiHist(sym: string, sink?: OiSink): Promise<OiPt[]> {
  const s = BYBIT_PERP[sym];
  if (!s) return [];
  const pts: OiPt[] = [];
  let cursor = "";
  for (let page = 0; page < 8; page++) {
    let url =
      `https://api.bybit.com/v5/market/open-interest?category=linear&symbol=${s}` +
      `&intervalTime=5min&limit=200`;
    if (cursor) url += `&cursor=${encodeURIComponent(cursor)}`;
    const raw = await fetchJson(url);
    const list = (raw as { result?: { list?: unknown[]; nextPageCursor?: string } } | null)
      ?.result?.list;
    const next = (raw as { result?: { nextPageCursor?: string } } | null)?.result?.nextPageCursor ?? "";
    if (!Array.isArray(list) || list.length === 0) break;
    for (const row of list) {
      if (!row || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      const ts = num(r.timestamp);
      const coin = num(r.openInterest);
      if (!(ts > 0) || !(coin > 0)) continue;
      pts.push({ ts, coin });
    }
    sink?.(sortPts(pts));
    if (!next || next === cursor) break;
    cursor = next;
  }
  return sortPts(pts);
}

async function okxOiHist(sym: string, sink?: OiSink): Promise<OiPt[]> {
  const s = OKX_PERP[sym];
  if (!s) return [];
  const pts: OiPt[] = [];
  let end: string | undefined;
  for (let page = 0; page < 12; page++) {
    let url =
      `https://www.okx.com/api/v5/rubik/stat/contracts/open-interest-history?instId=${s}&period=5m`;
    if (end) url += `&end=${end}`;
    const raw = await fetchJson(url);
    const data = (raw as { data?: unknown[] } | null)?.data;
    if (!Array.isArray(data) || data.length === 0) break;
    let oldest = "";
    for (const row of data) {
      if (!Array.isArray(row) || row.length < 4) continue;
      const ts = num(row[0]);
      const coin = num(row[2]);
      if (!(ts > 0) || !(coin > 0)) continue;
      pts.push({ ts, coin });
      oldest = String(row[0]);
    }
    if (!oldest || data.length < 50 || oldest === end) {
      sink?.(sortPts(pts));
      break;
    }
    sink?.(sortPts(pts));
    end = oldest;
  }
  return sortPts(pts);
}

async function gateOiHist(sym: string, sink?: OiSink): Promise<OiPt[]> {
  const contract = GATE_PERP[sym];
  if (!contract) return [];
  const pts: OiPt[] = [];
  let from: number | undefined;
  for (let page = 0; page < 4; page++) {
    let url =
      `https://api.gateio.ws/api/v4/futures/usdt/contract_stats?contract=${contract}` +
      `&interval=5m&limit=500`;
    if (from) url += `&from=${from}`;
    const raw = await fetchJson(url);
    if (!Array.isArray(raw) || raw.length === 0) break;
    let oldest = Infinity;
    for (const row of raw) {
      if (!row || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      const ts = num(r.time) * 1000;
      const contracts = num(r.open_interest);
      if (!(ts > 0) || !(contracts > 0)) continue;
      pts.push({ ts, coin: contracts * GATE_CTVAL });
      if (r.time != null && num(r.time) < oldest) oldest = num(r.time);
    }
    sink?.(sortPts(pts));
    if (!Number.isFinite(oldest) || raw.length < 50) break;
    const nextFrom = oldest - 500 * 300;
    if (from !== undefined && nextFrom >= from) break;
    from = nextFrom;
  }
  return sortPts(pts);
}

interface FundPt { ts: number; rate: number }
type FundSink = (pts: FundPt[]) => void;

function sortFundPts(pts: FundPt[]): FundPt[] {
  pts.sort((a, b) => a.ts - b.ts);
  const out: FundPt[] = [];
  for (const p of pts) {
    if (!(p.ts > 0) || !Number.isFinite(p.rate)) continue;
    if (out.length && out[out.length - 1].ts === p.ts)
      out[out.length - 1] = p;
    else out.push(p);
  }
  return out;
}

function mergeFund(parts: FundPt[][]): number[] {
  const maps = parts.map((s) => {
    const m = new Map<number, number>();
    for (const p of s) m.set(p.ts, p.rate);
    return m;
  });
  const times: number[] = [];
  const seen = new Set<number>();
  for (const m of maps) {
    for (const t of m.keys()) {
      if (!seen.has(t)) {
        seen.add(t);
        times.push(t);
      }
    }
  }
  times.sort((a, b) => a - b);
  if (times.length === 0) return [];
  const last: (number | null)[] = parts.map(() => null);
  const out: number[] = [];
  for (const t of times) {
    const vals: number[] = [];
    for (let i = 0; i < maps.length; i++) {
      const v = maps[i].get(t);
      if (v !== undefined) last[i] = v;
      if (last[i] !== null) vals.push(last[i]!);
    }
    if (vals.length === 0) continue;
    vals.sort((a, b) => a - b);
    out.push(t, vals[Math.floor(vals.length / 2)]);
  }
  const cap = MAX_SAMPLES * 2;
  return out.length > cap ? out.slice(out.length - cap) : out;
}

async function binanceFundHist(sym: string, sink?: FundSink): Promise<FundPt[]> {
  const s = USDT_PERP[sym];
  if (!s) return [];
  const pts: FundPt[] = [];
  let endTime: number | undefined;
  for (let page = 0; page < 4; page++) {
    let url =
      `https://fapi.binance.com/fapi/v1/fundingRate?symbol=${s}&limit=1000`;
    if (endTime) url += `&endTime=${endTime}`;
    const raw = await fetchJson(url);
    if (!Array.isArray(raw) || raw.length === 0) break;
    let oldest = Infinity;
    for (const row of raw) {
      if (!row || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      const ts = num(r.fundingTime);
      const rate = num(r.fundingRate);
      if (!(ts > 0) || !Number.isFinite(rate)) continue;
      pts.push({ ts, rate });
      if (ts < oldest) oldest = ts;
    }
    sink?.(sortFundPts(pts));
    if (!Number.isFinite(oldest) || raw.length < 200) break;
    endTime = oldest - 1;
  }
  return sortFundPts(pts);
}

async function bybitFundHist(sym: string, sink?: FundSink): Promise<FundPt[]> {
  const s = BYBIT_PERP[sym];
  if (!s) return [];
  const pts: FundPt[] = [];
  let cursor = "";
  for (let page = 0; page < 8; page++) {
    let url =
      `https://api.bybit.com/v5/market/funding/history?category=linear&symbol=${s}&limit=200`;
    if (cursor) url += `&cursor=${encodeURIComponent(cursor)}`;
    const raw = await fetchJson(url);
    const result = raw as { result?: { list?: unknown[]; nextPageCursor?: string } } | null;
    const list = result?.result?.list;
    const next = result?.result?.nextPageCursor ?? "";
    if (!Array.isArray(list) || list.length === 0) break;
    for (const row of list) {
      if (!row || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      const ts = num(r.fundingRateTimestamp);
      const rate = num(r.fundingRate);
      if (!(ts > 0) || !Number.isFinite(rate)) continue;
      pts.push({ ts, rate });
    }
    sink?.(sortFundPts(pts));
    if (!next || next === cursor) break;
    cursor = next;
  }
  return sortFundPts(pts);
}

async function okxFundHist(sym: string, sink?: FundSink): Promise<FundPt[]> {
  const s = OKX_PERP[sym];
  if (!s) return [];
  const pts: FundPt[] = [];
  let after: string | undefined;
  for (let page = 0; page < 8; page++) {
    let url =
      `https://www.okx.com/api/v5/public/funding-rate-history?instId=${s}&limit=100`;
    if (after) url += `&after=${after}`;
    const raw = await fetchJson(url);
    const data = (raw as { data?: unknown[] } | null)?.data;
    if (!Array.isArray(data) || data.length === 0) break;
    let oldest = "";
    for (const row of data) {
      if (!row || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      const ts = num(r.fundingTime);
      const rate = num(r.fundingRate);
      if (!(ts > 0) || !Number.isFinite(rate)) continue;
      pts.push({ ts, rate });
      oldest = String(r.fundingTime ?? "");
    }
    sink?.(sortFundPts(pts));
    if (!oldest || data.length < 50 || oldest === after) break;
    after = oldest;
  }
  return sortFundPts(pts);
}

async function gateFundHist(sym: string, sink?: FundSink): Promise<FundPt[]> {
  const contract = GATE_PERP[sym];
  if (!contract) return [];
  const raw = await fetchJson(
    `https://api.gateio.ws/api/v4/futures/usdt/funding_rate?contract=${contract}&limit=1000`,
  );
  if (!Array.isArray(raw)) return [];
  const pts: FundPt[] = [];
  for (const row of raw) {
    if (!row || typeof row !== "object") continue;
    const r = row as Record<string, unknown>;
    const ts = num(r.t) * 1000;
    const rate = num(r.r);
    if (!(ts > 0) || !Number.isFinite(rate)) continue;
    pts.push({ ts, rate });
  }
  const sorted = sortFundPts(pts);
  sink?.(sorted);
  return sorted;
}

async function hlFundHist(sym: string, sink?: FundSink): Promise<FundPt[]> {
  const start = Date.now() - 180 * 86400000;
  const raw = await postJson(HL_INFO, {
    type: "fundingHistory",
    coin: sym,
    startTime: start,
  });
  if (!Array.isArray(raw)) return [];
  const pts: FundPt[] = [];
  for (const row of raw) {
    if (!row || typeof row !== "object") continue;
    const r = row as Record<string, unknown>;
    const ts = num(r.time);
    const rate = num(r.fundingRate);
    if (!(ts > 0) || !Number.isFinite(rate)) continue;
    pts.push({ ts, rate });
  }
  const sorted = sortFundPts(pts);
  sink?.(sorted);
  return sorted;
}

async function backfillOi(): Promise<void> {
  const gen = liqGen;
  const sym = SYMBOLS[symIndex];
  const oiParts: OiPt[][] = [[], [], [], []];
  const fundParts: FundPt[][] = [[], [], [], [], []];
  const takeOi = (slot: number): OiSink => (pts) => {
    if (gen !== liqGen) return;
    oiParts[slot] = pts;
    const merged = mergeOi(oiParts);
    if (merged.length >= 2) {
      oiHist.length = 0;
      oiHist.push(...merged);
      post();
    }
  };
  const takeFund = (slot: number): FundSink => (pts) => {
    if (gen !== liqGen) return;
    fundParts[slot] = pts;
    const merged = mergeFund(fundParts);
    if (merged.length >= 2) {
      fundHist.length = 0;
      fundHist.push(...merged);
      post();
    }
  };

  // Non-Binance hist is enough to fill a 2h 1m chart on the first page.
  // Binance OI hist shares fapi weight with candle walk — wait until CLUSTER
  // has had its first burst so klines aren't starved.
  void bybitOiHist(sym, takeOi(1));
  void okxOiHist(sym, takeOi(2));
  void gateOiHist(sym, takeOi(3));
  void bybitFundHist(sym, takeFund(1));
  void okxFundHist(sym, takeFund(2));
  void gateFundHist(sym, takeFund(3));
  void hlFundHist(sym, takeFund(4));
  void (async () => {
    await new Promise((r) => setTimeout(r, 5000));
    if (gen !== liqGen) return;
    await whenFlowWalkQuiet(4000);
    if (gen !== liqGen) return;
    void binanceOiHist(sym, takeOi(0));
    void binanceFundHist(sym, takeFund(0));
  })();
}

function post(): void {
  const oi = packSeries(oiHist, oiLive, true);
  const funding = packSeries(fundHist, fundLive);
  // Full liq history only when prints arrived since the last post: the oi/
  // funding heartbeat fires every ~200ms and re-transfering (then re-sorting,
  // on the C++ side) 8k unchanged prints — plus the liqVersion bump it costs,
  // which invalidates every liq-gated consumer — is pure overhead. Live
  // prints arrive incrementally via marketLiq posts either way.
  const liq = liqPostedCount === liqCounter ? null : new Float64Array(liqHistory);
  liqPostedCount = liqCounter;
  (self as unknown as Worker).postMessage(
    { kind: "market", sym: symIndex, oi, funding, liq, liqBase },
    liq ? [oi.buffer, funding.buffer, liq.buffer]
        : [oi.buffer, funding.buffer],
  );
}

function publishAgg(): void {
  let oi = 0;
  let oiCount = 0;
  const rates: number[] = [];
  for (const s of snaps) {
    if (s.oi !== null) { oi += s.oi; ++oiCount; }
    if (s.funding !== null) rates.push(s.funding);
  }
  const ts = Date.now();
  if (oiCount >= 4 && oi > 0) {
    upsertLive(oiLive, ts, oi);
  }
  if (rates.length > 0) {
    rates.sort((a, b) => a - b);
    upsertLive(fundLive, ts, rates[Math.floor(rates.length / 2)]);
  }
  post();
}

function schedulePublish(): void {
  if (publishTimer) return;
  publishTimer = setTimeout(() => {
    publishTimer = null;
    publishAgg();
  }, 200);
}

async function poll(): Promise<void> {
  const gen = liqGen;
  const sym = SYMBOLS[symIndex];
  const results = await Promise.all(SOURCES.map((s) => s.fn(sym)));
  if (gen !== liqGen) return;
  for (let i = 0; i < results.length; i++) {
    setSnap(SOURCES[i].snap, results[i].oi, results[i].funding, false);
  }
  // Gate's liquidates WS is private; pull REST on the same interval.
  await backfillGate();
  await backfillHtx();
  if (gen !== liqGen) return;
  publishAgg();
}

async function pollBinanceOi(): Promise<void> {
  const gen = liqGen;
  const m = await binanceMarket(SYMBOLS[symIndex]);
  if (gen !== liqGen) return;
  setSnap(Snap.Binance, m.oi, m.funding);
}

const LIQ_SEEN_CAP = MAX_SAMPLES * 4; // bounded recent-event deduplication

function ingestLiq(source: string, price: number, qty: number, ts: number, side: number): boolean {
  if (!Number.isFinite(price) || !Number.isFinite(qty) || !Number.isFinite(ts) ||
      price <= 0 || qty <= 0 || ts <= 0 || !Number.isFinite(price * qty) ||
      (side !== 0 && side !== 1)) return false;

  const key = `${source}|${ts}|${price}|${qty}|${side}`;
  if (liqSeen.has(key)) return false;
  liqSeen.add(key);
  if (liqSeen.size > LIQ_SEEN_CAP) liqSeen.delete(liqSeen.values().next().value!);
  liqHistory.push(ts, price, qty, side);
  liqCounter += 1;
  if (liqHistory.length > MAX_SAMPLES * 4) {
    const drop = liqHistory.length - MAX_SAMPLES * 4;
    liqHistory.splice(0, drop);
    liqBase += drop / 4;
  }
  return true;
}

function recordLiq(source: string, price: number, qty: number, ts: number, side: number): void {
  const start = liqCounter;
  if (!ingestLiq(source, price, qty, ts, side)) return;
  const rows = new Float64Array([ts, price, qty, side]);
  (self as unknown as Worker).postMessage(
    { kind: "marketLiq", start, rows, sym: symIndex },
    [rows.buffer],
  );
}

// Forced-order side → panel contract: 0 = short liquidated, 1 = long liquidated.
function forceSide(raw: unknown): number {
  const s = String(raw ?? "").toLowerCase();
  return s === "buy" ? 0 : s === "sell" ? 1 : NaN;
}

// Position-side feeds (Bybit allLiquidation, Bitget UTA): Buy/buy = long liquidated.
function posSide(raw: unknown): number {
  const s = String(raw ?? "").toLowerCase();
  return s === "buy" ? 1 : s === "sell" ? 0 : NaN;
}

async function decodeWsData(data: unknown): Promise<string | null> {
  if (typeof data === "string") return data;
  let bytes: Uint8Array | null = null;
  if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
  else if (ArrayBuffer.isView(data))
    bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  else if (typeof Blob !== "undefined" && data instanceof Blob)
    bytes = new Uint8Array(await data.arrayBuffer());
  if (!bytes || bytes.length === 0) return null;
  if (bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b) {
    try {
      const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
      return await new Response(stream).text();
    } catch {
      return null;
    }
  }
  return new TextDecoder().decode(bytes);
}

function openPersistent(
  url: string,
  opts: {
    pingMs?: number;
    ping?: (ws: WebSocket) => void;
    onOpen?: (ws: WebSocket) => void;
    onMessage: (raw: string, ws: WebSocket) => void;
    binary?: boolean;
  },
): void {
  const gen = liqGen;
  let delay = 1000;
  const connect = (): void => {
    if (gen !== liqGen) return;
    const ws = new WebSocket(url);
    if (opts.binary) ws.binaryType = "arraybuffer";
    liqSockets.push(ws);
    let pingTimer: ReturnType<typeof setInterval> | null = null;
    ws.onopen = () => {
      delay = 1000;
      opts.onOpen?.(ws);
      if (opts.pingMs && opts.ping) {
        pingTimer = setInterval(() => {
          if (gen !== liqGen) return;
          if (ws.readyState === WebSocket.OPEN) opts.ping!(ws);
        }, opts.pingMs);
        liqTimers.push(pingTimer);
      }
    };
    ws.onmessage = (e: MessageEvent) => {
      void (async () => {
        if (gen !== liqGen) return;
        const text = await decodeWsData(e.data);
        if (text == null || gen !== liqGen) return;
        opts.onMessage(text, ws);
      })();
    };
    const retry = (): void => {
      if (pingTimer) {
        clearInterval(pingTimer);
        pingTimer = null;
      }
      const i = liqSockets.indexOf(ws);
      if (i >= 0) liqSockets.splice(i, 1);
      if (gen !== liqGen) return;
      const wait = delay;
      delay = Math.min(delay * 2, 30000);
      liqTimers.push(setTimeout(connect, wait));
    };
    ws.onerror = () => { try { ws.close(); } catch { /* noop */ } };
    ws.onclose = retry;
  };
  connect();
}

function ingestBinanceForce(raw: string, symbol: string, live: boolean): number {
  const msg = parseObj(raw);
  if (!msg) return 0;
  const payload = msg.data && typeof msg.data === "object" ? msg.data : msg;
  const events = Array.isArray(payload) ? payload
    : payload && typeof payload === "object" ? [payload] : [];
  let n = 0;
  for (const ev of events) {
    if (!ev || typeof ev !== "object") continue;
    const e = ev as Record<string, unknown>;
    if (typeof e.e === "string" && e.e !== "forceOrder") continue;
    const o = e.o && typeof e.o === "object" ? (e.o as Record<string, unknown>) : e;
    const sym = String(o.s ?? "");
    if (sym && sym !== symbol) continue;
    const px = num(o.ap) || num(o.p);
    const qty = num(o.z) || num(o.l) || num(o.q);
    const ts = num(o.T ?? e.E);
    const side = forceSide(o.S);
    if (live) recordLiq("binance-usdm", px, qty, ts, side);
    else if (ingestLiq("binance-usdm", px, qty, ts, side)) ++n;
  }
  return n;
}

function ingestBinanceCoinmForce(raw: string, symbol: string, contractUsd: number): void {
  const msg = parseObj(raw);
  if (!msg) return;
  const payload = msg.data && typeof msg.data === "object" ? msg.data : msg;
  const events = Array.isArray(payload) ? payload
    : payload && typeof payload === "object" ? [payload] : [];
  for (const ev of events) {
    if (!ev || typeof ev !== "object") continue;
    const e = ev as Record<string, unknown>;
    if (typeof e.e === "string" && e.e !== "forceOrder") continue;
    const o = e.o && typeof e.o === "object" ? (e.o as Record<string, unknown>) : e;
    const sym = String(o.s ?? "");
    if (sym && sym !== symbol) continue;
    const px = num(o.ap) || num(o.p);
    const contracts = num(o.z) || num(o.l) || num(o.q);
    const qty = px > 0 && contracts > 0 ? (contracts * contractUsd) / px : NaN;
    recordLiq("binance-coinm", px, qty, num(o.T ?? e.E), forceSide(o.S));
  }
}

function openBinanceLiq(): void {
  const s = USDT_PERP[SYMBOLS[symIndex]];
  if (!s) return;
  // USD-M market streams moved off /ws to /market/ws on 2026-04-23.
  // Legacy fstream.com/ws opens but never pushes forceOrder (or aggTrade).
  // Subscribe to the per-symbol stream only — the all-symbol !forceOrder@arr
  // is redundant and doubles bandwidth / event processing.
  const lower = s.toLowerCase();
  openPersistent(`wss://fstream.binance.com/market/stream?streams=${lower}@forceOrder`, {
    onMessage(raw) { ingestBinanceForce(raw, s, true); },
  });
  openPersistent(`wss://fstream.binance.com/market/stream?streams=${lower}@markPrice@1s`, {
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg) return;
      const d = msg.data && typeof msg.data === "object"
        ? (msg.data as Record<string, unknown>) : msg;
      if (d.e && d.e !== "markPriceUpdate") return;
      const funding = num(d.r);
      if (Number.isFinite(funding)) setSnap(Snap.Binance, null, funding);
    },
  });
  const coin = BINANCE_COINM[SYMBOLS[symIndex]];
  const ctUsd = BINANCE_COINM_USD[SYMBOLS[symIndex]];
  if (coin && ctUsd) {
    openPersistent("wss://dstream.binance.com/ws/!forceOrder@arr", {
      onMessage(raw) { ingestBinanceCoinmForce(raw, coin, ctUsd); },
    });
  }
}

function openBybitLiq(): void {
  const s = BYBIT_PERP[SYMBOLS[symIndex]];
  if (!s) return;
  openPersistent("wss://stream.bybit.com/v5/public/linear", {
    pingMs: BYBIT_PING_MS,
    ping: (ws) => { ws.send(JSON.stringify({ op: "ping" })); },
    onOpen(ws) {
      ws.send(JSON.stringify({
        op: "subscribe",
        args: [`allLiquidation.${s}`, `tickers.${s}`],
      }));
    },
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg) return;
      if (msg.op === "pong" || msg.op === "ping" || typeof msg.success === "boolean") return;
      if (typeof msg.topic === "string" && msg.topic.startsWith("tickers.")) {
        const d = msg.data && typeof msg.data === "object"
          ? (msg.data as Record<string, unknown>) : null;
        if (!d) return;
        const oi = num(d.openInterest);
        const funding = num(d.fundingRate);
        setSnap(Snap.Bybit,
          Number.isFinite(oi) && oi >= 0 ? oi : null,
          Number.isFinite(funding) ? funding : null);
        return;
      }
      if (typeof msg.topic !== "string" || !msg.topic.startsWith("allLiquidation.")) return;
      const rows = Array.isArray(msg.data) ? msg.data : msg.data ? [msg.data] : [];
      for (const row of rows) {
        if (!row || typeof row !== "object") continue;
        const o = row as Record<string, unknown>;
        // allLiquidation.S is the liquidated position side: Buy = long liquidated.
        recordLiq("bybit", num(o.p ?? o.price), num(o.v ?? o.size),
                  num(o.T ?? o.updatedTime), posSide(o.S ?? o.side));
      }
    },
  });
}

function recordOkxDetails(details: unknown, ctVal: number): number {
  if (!Array.isArray(details)) return 0;
  let n = 0;
  for (const row of details) {
    if (!row || typeof row !== "object") continue;
    const o = row as Record<string, unknown>;
    const pos = String(o.posSide ?? "").toLowerCase();
    const side = pos === "long" ? 1 : pos === "short" ? 0 : forceSide(o.side);
    if (ingestLiq("okx", num(o.bkPx ?? o.px), num(o.sz) * ctVal, num(o.ts ?? o.time), side))
      ++n;
  }
  return n;
}

function openOkxLiq(): void {
  const instId = OKX_PERP[SYMBOLS[symIndex]];
  const ctVal = OKX_CTVAL[SYMBOLS[symIndex]] ?? 1;
  if (!instId) return;
  openPersistent("wss://ws.okx.com:8443/ws/v5/public", {
    pingMs: OKX_PING_MS,
    ping: (ws) => { ws.send("ping"); },
    onOpen(ws) {
      ws.send(JSON.stringify({
        op: "subscribe",
        args: [
          { channel: "liquidation-orders", instType: "SWAP" },
          { channel: "open-interest", instId },
          { channel: "funding-rate", instId },
        ],
      }));
    },
    onMessage(raw, ws) {
      // OKX/Bitget close the socket ~30s in unless the server PING gets a
      // PONG — observed live as close 4004 churn without this reply.
      if (raw === "ping" || raw === "PING") {
        ws.send("pong");
        return;
      }
      if (raw === "pong" || raw === "PONG") return;
      const msg = parseObj(raw);
      if (!msg) return;
      const arg = msg.arg && typeof msg.arg === "object"
        ? (msg.arg as Record<string, unknown>) : null;
      if (arg && arg.channel === "open-interest") {
        const rows = Array.isArray(msg.data) ? msg.data : [];
        for (const row of rows) {
          if (!row || typeof row !== "object") continue;
          const o = row as Record<string, unknown>;
          if (o.instId && o.instId !== instId) continue;
          const oi = num(o.oiCcy);
          setSnap(Snap.Okx, Number.isFinite(oi) && oi >= 0 ? oi : null, null);
        }
        return;
      }
      if (arg && arg.channel === "funding-rate") {
        const rows = Array.isArray(msg.data) ? msg.data : [];
        for (const row of rows) {
          if (!row || typeof row !== "object") continue;
          const o = row as Record<string, unknown>;
          if (o.instId && o.instId !== instId) continue;
          const f = num(o.fundingRate);
          setSnap(Snap.Okx, null, Number.isFinite(f) ? f : null);
        }
        return;
      }
      if (arg && arg.channel !== "liquidation-orders") return;
      const groups = Array.isArray(msg.data) ? msg.data : [];
      for (const group of groups) {
        if (!group || typeof group !== "object") continue;
        const g = group as Record<string, unknown>;
        if (g.instId !== instId) continue;
        if (!Array.isArray(g.details)) continue;
        for (const row of g.details) {
          if (!row || typeof row !== "object") continue;
          const o = row as Record<string, unknown>;
          const pos = String(o.posSide ?? "").toLowerCase();
          const side = pos === "long" ? 1 : pos === "short" ? 0 : forceSide(o.side);
          recordLiq("okx", num(o.bkPx ?? o.px), num(o.sz) * ctVal, num(o.ts ?? o.time), side);
        }
      }
    },
  });
}

async function backfillOkx(): Promise<number> {
  const instId = OKX_PERP[SYMBOLS[symIndex]];
  const ctVal = OKX_CTVAL[SYMBOLS[symIndex]] ?? 1;
  if (!instId) return 0;
  const uly = instId.replace(/-SWAP$/, "");
  const raw = await fetchJson(
    `https://www.okx.com/api/v5/public/liquidation-orders?instType=SWAP&uly=${uly}&state=filled&limit=100`,
  );
  if (!raw) return 0;
  const data = (raw as { data?: unknown[] }).data;
  if (!Array.isArray(data)) return 0;
  let added = 0;
  for (const group of data) {
    if (!group || typeof group !== "object") continue;
    const g = group as Record<string, unknown>;
    if (g.instId !== instId) continue;
    added += recordOkxDetails(g.details ?? g, ctVal);
  }
  return added;
}

function openBitgetLiq(): void {
  const s = USDT_PERP[SYMBOLS[symIndex]];
  if (!s) return;
  openPersistent("wss://ws.bitget.com/v2/ws/public", {
    pingMs: BYBIT_PING_MS,
    ping: (ws) => { ws.send("ping"); },
    onOpen(ws) {
      ws.send(JSON.stringify({
        op: "subscribe",
        args: [{ instType: "USDT-FUTURES", channel: "ticker", instId: s }],
      }));
    },
    onMessage(raw, ws) {
      if (raw === "ping" || raw === "PING") {
        ws.send("pong");
        return;
      }
      if (raw === "pong" || raw === "PONG") return;
      const msg = parseObj(raw);
      if (!msg) return;
      const arg = msg.arg && typeof msg.arg === "object"
        ? (msg.arg as Record<string, unknown>) : null;
      if (!arg || arg.channel !== "ticker") return;
      const rows = Array.isArray(msg.data) ? msg.data : [];
      for (const row of rows) {
        if (!row || typeof row !== "object") continue;
        const o = row as Record<string, unknown>;
        if (typeof o.symbol === "string" && o.symbol !== s) continue;
        const oi = num(o.holdingAmount ?? o.openInterest);
        const funding = num(o.fundingRate ?? o.capitalRate);
        setSnap(Snap.Bitget,
          Number.isFinite(oi) && oi >= 0 ? oi : null,
          Number.isFinite(funding) ? funding : null);
      }
    },
  });
  // Classic v2 has no liquidation channel (UTA upgrade). v3 `side` is
  // position side: buy = long liquidated, sell = short liquidated.
  openPersistent("wss://ws.bitget.com/v3/ws/public", {
    pingMs: BYBIT_PING_MS,
    ping: (ws) => { ws.send("ping"); },
    onOpen(ws) {
      ws.send(JSON.stringify({
        op: "subscribe",
        args: [{ instType: "usdt-futures", topic: "liquidation" }],
      }));
    },
    onMessage(raw, ws) {
      if (raw === "ping" || raw === "PING") {
        ws.send("pong");
        return;
      }
      if (raw === "pong" || raw === "PONG") return;
      const msg = parseObj(raw);
      if (!msg) return;
      const arg = msg.arg && typeof msg.arg === "object"
        ? (msg.arg as Record<string, unknown>) : null;
      const topic = arg ? String(arg.topic ?? arg.channel ?? "") : "";
      if (topic !== "liquidation") return;
      const rows = Array.isArray(msg.data) ? msg.data : [];
      for (const row of rows) {
        if (!row || typeof row !== "object") continue;
        const o = row as Record<string, unknown>;
        if (typeof o.symbol === "string" && o.symbol !== s) continue;
        recordLiq("bitget", num(o.price), num(o.amount), num(o.ts), posSide(o.side));
      }
    },
  });
}

function openDeribitLiq(): void {
  const inst = DERIBIT_PERP[SYMBOLS[symIndex]];
  if (!inst) return;
  const channel = `trades.${inst}.100ms`;
  openPersistent("wss://www.deribit.com/ws/api/v2", {
    onOpen(ws) {
      ws.send(JSON.stringify({
        jsonrpc: "2.0", id: 1, method: "public/subscribe",
        params: { channels: [channel, `ticker.${inst}.100ms`] },
      }));
    },
    onMessage(raw, ws) {
      const msg = parseObj(raw);
      if (!msg) return;
      if (msg.method === "test_request") {
        ws.send(JSON.stringify({
          jsonrpc: "2.0",
          id: typeof msg.id === "number" ? msg.id : 0,
          method: "public/test",
          params: {},
        }));
        return;
      }
      if (msg.method !== "subscription" || !msg.params || typeof msg.params !== "object") return;
      const params = msg.params as Record<string, unknown>;
      if (typeof params.channel === "string" && params.channel.startsWith("ticker.")) {
        const rows = Array.isArray(params.data) ? params.data
          : params.data && typeof params.data === "object" ? [params.data] : [];
        for (const row of rows) {
          if (!row || typeof row !== "object") continue;
          const o = row as Record<string, unknown>;
          const usd = num(o.open_interest);
          const mark = num(o.mark_price ?? o.mark);
          const funding = num(o.current_funding);
          setSnap(Snap.Deribit,
            Number.isFinite(usd) && usd >= 0 && mark > 0 ? (inst.includes("_USDC") ? usd : usd / mark) : null,
            Number.isFinite(funding) ? funding : null);
        }
        return;
      }
      if (params.channel !== channel || !Array.isArray(params.data)) return;
      for (const row of params.data) {
        if (!row || typeof row !== "object") continue;
        const o = row as Record<string, unknown>;
        if (o.liquidation === undefined || o.liquidation === null) continue;
        const px = num(o.price);
        const amt = num(o.amount);
        // Inverse perp amount is USD; convert to base like the Deribit tape.
        if (!(px > 0) || !(amt > 0)) continue;
        // `direction` is the taker's side. "T" = taker was liquidated;
        // "M" = maker was, so the liquidated side is the opposite.
        const flag = String(o.liquidation);
        let side = forceSide(o.direction);
        if (flag !== "M" && flag !== "T" && flag !== "MT") continue;
        if (!Number.isFinite(side)) continue;
        if (flag === "M") side = 1 - side;
        recordLiq("deribit", px, inst.includes("_USDC") ? amt : amt / px, num(o.timestamp), side);
      }
    },
  });
}

function ingestHtxRow(row: unknown, code: string, ctVal: number, live: boolean): boolean {
  if (!row || typeof row !== "object") return false;
  const o = row as Record<string, unknown>;
  const inst = String(o.contract_code ?? o.symbol ?? "");
  if (inst && inst !== code) return false;
  const px = num(o.bankrupt_price ?? o.price);
  const turnover = num(o.trade_turnover);
  const token = num(o.amount);
  const contracts = num(o.volume);
  const qty = turnover > 0 && px > 0 ? turnover / px
    : token > 0 && !(Math.abs(token - contracts) < 1e-9) ? token
    : contracts * ctVal;
  const ts = num(o.liquidation_time ?? o.created_at ?? o.ts ?? o.t);
  const pos = String(o.position_side ?? "").toLowerCase();
  const side = pos === "long" ? 1 : pos === "short" ? 0 : forceSide(o.side ?? o.direction);
  if (live) {
    const before = liqCounter;
    recordLiq("htx", px, qty, ts, side);
    return liqCounter > before;
  }
  return ingestLiq("htx", px, qty, ts, side);
}

function ingestHtxPayload(msg: unknown, code: string, ctVal: number, live: boolean): number {
  let n = 0;
  const walk = (v: unknown): void => {
    if (!v) return;
    if (Array.isArray(v)) { for (const x of v) walk(x); return; }
    if (typeof v !== "object") return;
    const o = v as Record<string, unknown>;
    if (Array.isArray(o.orders)) { walk(o.orders); return; }
    if (Array.isArray(o.data) && o.volume == null && o.amount == null && o.trade_turnover == null) {
      walk(o.data);
      return;
    }
    if (o.volume != null || o.amount != null || o.trade_turnover != null || o.bankrupt_price != null) {
      if (ingestHtxRow(o, code, ctVal, live)) ++n;
    }
  };
  walk(msg);
  return n;
}

async function backfillHtx(windows = 1): Promise<number> {
  const code = HTX_PERP[SYMBOLS[symIndex]];
  const ctVal = HTX_CTVAL[SYMBOLS[symIndex]] ?? 1;
  if (!code) return 0;
  const now = Date.now();
  const span = 2 * 3600 * 1000;
  let n = 0;
  for (let i = 0; i < windows; ++i) {
    const end = now - i * span;
    const start = end - span;
    let from = "";
    for (let page = 0; page < 5; ++page) {
      const raw = await fetchJson(
        `https://api.hbdm.com/v5/market/liquidation_orders?contract_code=${code}` +
        `&start_time=${start}&end_time=${end}&limit=100&direct=prev` +
        (from ? `&from=${encodeURIComponent(from)}` : ""),
      );
      if (!raw || typeof raw !== "object") break;
      const data = (raw as { data?: unknown }).data;
      n += ingestHtxPayload({ data }, code, ctVal, false);
      if (!Array.isArray(data) || data.length < 100) break;
      const last = data[data.length - 1];
      const id = last && typeof last === "object"
        ? String((last as Record<string, unknown>).id ?? "") : "";
      if (!id || id === from) break;
      from = id;
    }
  }
  return n;
}

function openHtxLiq(): void {
  const code = HTX_PERP[SYMBOLS[symIndex]];
  const ctVal = HTX_CTVAL[SYMBOLS[symIndex]] ?? 1;
  if (!code) return;
  openPersistent("wss://api.hbdm.com/linear-swap-notification", {
    binary: true,
    onOpen(ws) {
      ws.send(JSON.stringify({ op: "sub", topic: `public.${code}.liquidation_orders` }));
    },
    onMessage(raw, ws) {
      const msg = parseObj(raw);
      if (!msg) return;
      if (msg.op === "ping") {
        ws.send(JSON.stringify({ op: "pong", ts: msg.ts }));
        return;
      }
      if (typeof msg.topic === "string" && !msg.topic.includes("liquidation")) return;
      ingestHtxPayload(msg, code, ctVal, true);
    },
  });
}

function openAsterLiq(): void {
  const s = USDT_PERP[SYMBOLS[symIndex]];
  if (!s) return;
  openPersistent(`wss://fstream.asterdex.com/ws/${s.toLowerCase()}@forceOrder`, {
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg || msg.e !== "forceOrder") return;
      const o = msg.o && typeof msg.o === "object"
        ? (msg.o as Record<string, unknown>) : msg;
      recordLiq("aster", num(o.p), num(o.q), num(o.T), forceSide(o.S));
    },
  });
}

function ingestBitgetRows(list: unknown, symbol: string): number {
  if (!Array.isArray(list)) return 0;
  let n = 0;
  for (const row of list) {
    if (!row || typeof row !== "object") continue;
    const o = row as Record<string, unknown>;
    if (typeof o.symbol === "string" && o.symbol !== symbol) continue;
    if (ingestLiq("bitget", num(o.price), num(o.amount), num(o.ts), posSide(o.side)))
      ++n;
  }
  return n;
}

function ingestGateRows(list: unknown): number {
  if (!Array.isArray(list)) return 0;
  let n = 0;
  for (const row of list) {
    if (!row || typeof row !== "object") continue;
    const o = row as Record<string, unknown>;
    const px = num(o.fill_price ?? o.order_price);
    const sz = num(o.size);
    let ts = num(o.time_ms ?? o.time);
    if (ts > 0 && ts < 1e12) ts *= 1000;
    // `size` is user position size: positive = long, negative = short.
    if (ingestLiq("gate", px, Math.abs(sz) * GATE_CTVAL, ts, sz < 0 ? 0 : 1)) ++n;
  }
  return n;
}

async function backfillBitget(): Promise<number> {
  const s = USDT_PERP[SYMBOLS[symIndex]];
  if (!s) return 0;
  const raw = await fetchJson(
    `https://api.bitget.com/api/v3/market/liquidations?category=USDT-FUTURES&symbol=${s}&limit=100`,
  );
  // v3 wraps results in data.list; keep the array fallback if the envelope shifts.
  const list = (raw as { data?: { list?: unknown[] } | unknown[] } | null)?.data;
  const rows = Array.isArray(list) ? list
    : list && typeof list === "object" ? (list as { list?: unknown[] }).list ?? [] : [];
  return ingestBitgetRows(rows, s);
}

async function backfillGate(): Promise<number> {
  const s = GATE_PERP[SYMBOLS[symIndex]];
  if (!s) return 0;
  const raw = await fetchJson(
    `https://api.gateio.ws/api/v4/futures/usdt/liq_orders?contract=${s}&limit=100`,
  );
  return ingestGateRows(raw);
}

function bitfinexWanted(symbol: string): boolean {
  const want = BITFINEX_PERP[SYMBOLS[symIndex]];
  return typeof symbol === "string" && !!want && symbol === want;
}

function ingestBitfinexRow(row: unknown): boolean {
  if (!Array.isArray(row) || row[0] !== "pos") return false;
  // [8] is_match: 0 = trigger, 1 = market execution. Keep executions only.
  if (num(row[8]) !== 1) return false;
  const symbol = String(row[4] ?? "");
  if (!bitfinexWanted(symbol)) return false;
  const amt = num(row[5]);
  const px = num(row[11] ?? row[6]);
  const ts = num(row[2]);
  if (!(px > 0) || !(Math.abs(amt) > 0)) return false;
  // Amount is position size: positive = long, negative = short.
  return ingestLiq("bitfinex", px, Math.abs(amt), ts, amt < 0 ? 0 : 1);
}

function recordBitfinexRow(row: unknown): void {
  if (!Array.isArray(row) || row[0] !== "pos") return;
  if (num(row[8]) !== 1) return;
  if (!bitfinexWanted(String(row[4] ?? ""))) return;
  const amt = num(row[5]);
  const px = num(row[11] ?? row[6]);
  recordLiq("bitfinex", px, Math.abs(amt), num(row[2]), amt < 0 ? 0 : 1);
}

function openBitfinexLiq(): void {
  if (!BITFINEX_PERP[SYMBOLS[symIndex]]) return;
  openPersistent("wss://api-pub.bitfinex.com/ws/2", {
    onOpen(ws) {
      ws.send(JSON.stringify({ event: "subscribe", channel: "status", key: "liq:global" }));
    },
    onMessage(raw) {
      let msg: unknown;
      try { msg = JSON.parse(raw); } catch { return; }
      if (!Array.isArray(msg) || msg[1] === "hb") return;
      const body = msg[1];
      if (Array.isArray(body) && body.length && Array.isArray(body[0])) {
        for (const row of body) recordBitfinexRow(row);
        return;
      }
      if (Array.isArray(body)) recordBitfinexRow(body);
    },
  });
}

function recordLighterTrade(row: unknown): void {
  if (!row || typeof row !== "object") return;
  const o = row as Record<string, unknown>;
  if (String(o.type ?? "") !== "liquidation") return;
  const px = num(o.price);
  const sz = num(o.size);
  // is_maker_ask: taker bought → forced buy → short liquidated.
  recordLiq("lighter", px, sz, num(o.timestamp ?? o.transaction_time),
            o.is_maker_ask ? 0 : 1);
}

function openLighterLiq(): void {
  const market = LIGHTER_MARKET[SYMBOLS[symIndex]];
  if (market === undefined) return;
  openPersistent("wss://mainnet.zklighter.elliot.ai/stream?readonly=true", {
    pingMs: BYBIT_PING_MS,
    ping: (ws) => { ws.send(JSON.stringify({ type: "ping" })); },
    onOpen(ws) {
      ws.send(JSON.stringify({ type: "subscribe", channel: `trade/${market}` }));
    },
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg) return;
      if (Array.isArray(msg.liquidation_trades))
        for (const row of msg.liquidation_trades) recordLighterTrade(row);
      if (Array.isArray(msg.trades))
        for (const row of msg.trades) recordLighterTrade(row);
    },
  });
}

function openDydxLiq(): void {
  const id = DYDX_PERP[SYMBOLS[symIndex]];
  if (!id) return;
  openPersistent("wss://indexer.dydx.trade/v4/ws", {
    onOpen(ws) {
      ws.send(JSON.stringify({ type: "subscribe", channel: "v4_trades", id }));
      ws.send(JSON.stringify({ type: "subscribe", channel: "v4_markets" }));
    },
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg) return;
      if (msg.channel === "v4_markets") {
        const contents = msg.contents && typeof msg.contents === "object"
          ? (msg.contents as Record<string, unknown>) : null;
        const markets = contents && contents.markets && typeof contents.markets === "object"
          ? (contents.markets as Record<string, unknown>)
          : contents;
        const m = markets && typeof markets === "object"
          ? (markets as Record<string, unknown>)[id] as Record<string, unknown> | undefined
          : undefined;
        if (m) {
          const oi = num(m.openInterest);
          const funding = num(m.nextFundingRate ?? m.annualizedFunding);
          setSnap(Snap.Dydx,
            Number.isFinite(oi) && oi >= 0 ? oi : null,
            Number.isFinite(funding) ? funding : null);
        }
        return;
      }
      const contents = msg.contents && typeof msg.contents === "object"
        ? (msg.contents as Record<string, unknown>) : null;
      const trades = contents && Array.isArray(contents.trades) ? contents.trades
        : Array.isArray(msg.contents) ? msg.contents : [];
      for (const row of trades) {
        if (!row || typeof row !== "object") continue;
        const o = row as Record<string, unknown>;
        const kind = String(o.type ?? "").toUpperCase();
        if (kind !== "LIQUIDATED" && kind !== "DELEVERAGED") continue;
        const ts = Date.parse(String(o.createdAt ?? ""));
        recordLiq("dydx", num(o.price), num(o.size), ts,
                  String(o.side ?? "").toUpperCase() === "SELL" ? 1 : 0);
      }
    },
  });
}

function openExtendedLiq(): void {
  const id = DYDX_PERP[SYMBOLS[symIndex]];
  if (!id) return;
  openPersistent(
    `wss://api.starknet.extended.exchange/stream.extended.exchange/v1/publicTrades/${id}`,
    {
      onMessage(raw) {
        const msg = parseObj(raw);
        if (!msg || !Array.isArray(msg.data)) return;
        for (const row of msg.data) {
          if (!row || typeof row !== "object") continue;
          const o = row as Record<string, unknown>;
          const kind = String(o.tT ?? "").toUpperCase();
          if (kind !== "LIQUIDATION" && kind !== "LIQUIDATED") continue;
          recordLiq("extended", num(o.p), num(o.q), num(o.T), forceSide(o.S));
        }
      },
    },
  );
}

function hlCoin(): string {
  return SYMBOLS[symIndex] ?? "";
}

function hlLiqSide(dir: unknown, side: unknown): number {
  // `dir` is victim language even on backstop vault fills
  // ("Liquidated Isolated Short" + side A = vault sold to take the short).
  const d = String(dir ?? "");
  if (/short/i.test(d)) return 0;
  if (/long/i.test(d)) return 1;
  return String(side ?? "").toUpperCase() === "A" ? 0 : 1;
}

function isHlLiqFill(o: Record<string, unknown>, coin: string): boolean {
  if (String(o.coin ?? "") !== coin) return false;
  if (o.liquidation && typeof o.liquidation === "object") return true;
  return String(o.dir ?? "").startsWith("Liquidated");
}

function ingestHlFill(row: unknown): boolean {
  if (!row || typeof row !== "object") return false;
  const o = row as Record<string, unknown>;
  if (!isHlLiqFill(o, hlCoin())) return false;
  return ingestLiq("hyperliquid", num(o.px), num(o.sz), num(o.time), hlLiqSide(o.dir, o.side));
}

function recordHlFills(rows: unknown, since = 0): void {
  if (!Array.isArray(rows)) return;
  for (const row of rows) {
    if (!row || typeof row !== "object") continue;
    const o = row as Record<string, unknown>;
    if (since > 0 && num(o.time) < since) continue;
    if (!isHlLiqFill(o, hlCoin())) continue;
    recordLiq("hyperliquid", num(o.px), num(o.sz), num(o.time), hlLiqSide(o.dir, o.side));
  }
}

const hlFillLookup = new Set<string>();

async function lookupHlUserFills(user: string, since: number): Promise<void> {
  const addr = user.toLowerCase();
  const key = `${addr}|${since}`;
  if (!/^0x[a-f0-9]{40}$/.test(addr) || hlFillLookup.has(key)) return;
  hlFillLookup.add(key);
  const gen = liqGen;
  const raw = await postJson(HL_INFO, { type: "userFills", user: addr });
  if (gen !== liqGen) return;
  recordHlFills(raw, since);
}

function hlAssetFromRequest(req: unknown): number | null {
  if (!req || typeof req !== "object") return null;
  const o = req as Record<string, unknown>;
  const iso = o.Isolated && typeof o.Isolated === "object"
    ? (o.Isolated as Record<string, unknown>) : null;
  const cross = o.Cross && typeof o.Cross === "object"
    ? (o.Cross as Record<string, unknown>) : null;
  const n = num(iso?.asset ?? cross?.asset ?? o.asset);
  return Number.isFinite(n) ? n : null;
}

function hlLiquidateMatches(action: Record<string, unknown>): boolean {
  const want = HL_COIN[hlCoin()];
  if (want === undefined) return false;
  const reqs = [action.request, ...(Array.isArray(action.requests) ? action.requests : [])];
  let sawAsset = false;
  for (const req of reqs) {
    const asset = hlAssetFromRequest(req);
    if (asset === null) continue;
    sawAsset = true;
    if (asset === want) return true;
  }
  // Cross takeovers can omit a per-asset target — pull victim fills and filter.
  return !sawAsset;
}

function openHyperliquidLiq(): void {
  hlFillLookup.clear();
  openPersistent(HL_WS, {
    pingMs: HL_PING_MS,
    ping: (ws) => { ws.send(JSON.stringify({ method: "ping" })); },
    onOpen(ws) {
      for (const user of HL_LIQUIDATORS) {
        ws.send(JSON.stringify({ method: "subscribe", subscription: { type: "userFills", user } }));
      }
      const coin = hlCoin();
      if (coin)
        ws.send(JSON.stringify({
          method: "subscribe",
          subscription: { type: "activeAssetCtx", coin },
        }));
    },
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg) return;
      if (msg.channel === "subscriptionResponse" || msg.channel === "pong") return;
      if (msg.channel === "activeAssetCtx" || msg.channel === "activeSpotAssetCtx") {
        const data = msg.data && typeof msg.data === "object"
          ? (msg.data as Record<string, unknown>) : null;
        const ctx = data && data.ctx && typeof data.ctx === "object"
          ? (data.ctx as Record<string, unknown>)
          : data;
        const coin = String(data?.coin ?? "");
        if (ctx && (!coin || coin === hlCoin())) {
          const oi = num(ctx.openInterest);
          const funding = num(ctx.funding);
          setSnap(Snap.Hyperliquid,
            Number.isFinite(oi) && oi >= 0 ? oi : null,
            Number.isFinite(funding) ? funding : null);
        }
        return;
      }
      if (msg.channel !== "userFills" && msg.channel !== "user") return;
      const data = msg.data && typeof msg.data === "object"
        ? (msg.data as Record<string, unknown>) : null;
      // Snapshots replay up to 2000 fills, often months old. Live incremental
      // + userFillsByTime backfill cover the window we actually want.
      if (data?.isSnapshot) return;
      recordHlFills(data && Array.isArray(data.fills) ? data.fills : msg.data);
    },
  });
  openPersistent(HL_RPC_WS, {
    pingMs: HL_PING_MS,
    ping: (ws) => { ws.send(JSON.stringify({ method: "ping" })); },
    onOpen(ws) {
      ws.send(JSON.stringify({ method: "subscribe", subscription: { type: "explorerTxs" } }));
    },
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg || msg.channel === "subscriptionResponse" || msg.channel === "error") return;
      const rows = Array.isArray(msg.data) ? msg.data : [];
      for (const row of rows) {
        if (!row || typeof row !== "object") continue;
        const tx = row as Record<string, unknown>;
        const action = tx.action && typeof tx.action === "object"
          ? (tx.action as Record<string, unknown>) : null;
        if (!action || action.type !== "liquidate") continue;
        if (!hlLiquidateMatches(action)) continue;
        const victim = String(action.user ?? "");
        const vault = String(action.defaultLiquidator ?? "");
        const since = Math.max(0, num(tx.time) - 8000);
        void lookupHlUserFills(victim, since);
        if (vault) void lookupHlUserFills(vault, since);
      }
    },
  });
}

async function backfillHyperliquid(): Promise<number> {
  const startTime = Date.now() - 2 * 86400000;
  const batches = await Promise.all(HL_LIQUIDATORS.map((user) =>
    postJson(HL_INFO, { type: "userFillsByTime", user, startTime }),
  ));
  let n = 0;
  for (const raw of batches) {
    if (!Array.isArray(raw)) continue;
    for (const row of raw) if (ingestHlFill(row)) ++n;
  }
  return n;
}

async function backfillBitfinex(): Promise<number> {
  const raw = await fetchJson("https://api-pub.bitfinex.com/v2/liquidations/hist?limit=120");
  if (!Array.isArray(raw)) return 0;
  let n = 0;
  for (const wrap of raw) {
    const row = Array.isArray(wrap) ? wrap[0] : wrap;
    if (ingestBitfinexRow(row)) ++n;
  }
  return n;
}

async function backfillAll(): Promise<void> {
  const gen = liqGen;
  const added = (await Promise.all([
    backfillOkx(), backfillBitget(), backfillGate(), backfillBitfinex(),
    backfillHyperliquid(), backfillHtx(12),
  ])).reduce((a, b) => a + b, 0);
  if (gen === liqGen && added > 0) post();
}

function openGateTickers(): void {
  const contract = GATE_PERP[SYMBOLS[symIndex]];
  if (!contract) return;
  openPersistent(GATE_WS, {
    pingMs: GATE_PING_MS,
    ping: (ws) => {
      ws.send(JSON.stringify({ time: Math.floor(Date.now() / 1000), channel: "futures.ping" }));
    },
    onOpen(ws) {
      ws.send(JSON.stringify({
        time: Math.floor(Date.now() / 1000),
        channel: "futures.tickers",
        event: "subscribe",
        payload: [contract],
      }));
    },
    onMessage(raw) {
      const msg = parseObj(raw);
      if (!msg || msg.channel !== "futures.tickers" || msg.event === "subscribe") return;
      const rows = Array.isArray(msg.result) ? msg.result
        : msg.result && typeof msg.result === "object" ? [msg.result] : [];
      for (const row of rows) {
        if (!row || typeof row !== "object") continue;
        const o = row as Record<string, unknown>;
        if (typeof o.contract === "string" && o.contract !== contract) continue;
        const size = num(o.total_size);
        const funding = num(o.funding_rate);
        setSnap(Snap.Gate,
          Number.isFinite(size) && size >= 0 ? size * GATE_CTVAL : null,
          Number.isFinite(funding) ? funding : null);
      }
    },
  });
}

function openLiq(): void {
  openBinanceLiq();
  openBybitLiq();
  openOkxLiq();
  openBitgetLiq();
  openDeribitLiq();
  openAsterLiq();
  openHtxLiq();
  openBitfinexLiq();
  openLighterLiq();
  openDydxLiq();
  openExtendedLiq();
  openHyperliquidLiq();
  openGateTickers();
  void backfillAll();
}

export function startMarketFeed(sym: number): void {
  stopMarketFeed();
  symIndex = sym;
  oiHist.length = 0;
  oiLive.length = 0;
  fundHist.length = 0;
  fundLive.length = 0;
  liqHistory.length = 0;
  liqSeen.clear();
  liqCounter = 0;
  liqBase = 0;
  liqPostedCount = -1;
  clearSnaps();
  openLiq();
  const gen = liqGen;
  void (async () => {
    await new Promise((r) => setTimeout(r, 1500));
    if (gen !== liqGen) return;
    await poll();
    if (gen !== liqGen) return;
    timer = setInterval(poll, POLL_MS);
    binanceTimer = setInterval(() => { void pollBinanceOi(); }, BINANCE_OI_MS);
  })();
  void backfillOi();
}

export function stopMarketFeed(): void {
  liqGen += 1;
  hlFillLookup.clear();
  if (timer) { clearInterval(timer); timer = null; }
  if (binanceTimer) { clearInterval(binanceTimer); binanceTimer = null; }
  if (publishTimer) { clearTimeout(publishTimer); publishTimer = null; }
  for (const t of liqTimers) clearTimeout(t);
  liqTimers.length = 0;
  for (const ws of liqSockets) {
    try { ws.onclose = null; ws.close(); } catch { /* noop */ }
  }
  liqSockets.length = 0;
}
