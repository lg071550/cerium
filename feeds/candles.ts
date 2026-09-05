// Binance USD-M candle history for the chart panel.
// Posted to the main thread as transferable packed arrays: OHLCV bars plus,
// when requested by a footprint chart, exact [ts, price, qty, aggressor side]
// aggTrades. Ordinary candle charts do not pay the history/network cost.
//
// Time bars come from the klines API: standard intervals directly, custom
// minute counts by aggregating the largest standard interval that evenly
// divides the target (paged backwards so the 1000-bar chart cap still fills).
// Tick/volume bars can't come from klines — they are bootstrapped from the
// aggTrades endpoint, paged backwards via endTime.

export const TF_TIME = 0;
export const TF_TICK = 1;
export const TF_VOLUME = 2;

const MAX_BARS = 2000; // enough for a complete 1m UTC TPO session
const KLINE_PAGE = 1000;
const MAX_KLINE_PAGES = 10;
const AGG_PAGE = 1000;
// Exact aggressor prints for CLUSTER/PROFILE. Klines still load MAX_BARS, but
// Binance only serves aggTrades 1000-at-a-time (weight 20). A flat 80k window
// is ~90 1m bars / ~17 5m bars on a busy BTC tape, so a zoomed CLUSTER pane
// was half empty OHLC. Size the pull to ~TARGET_FLOW_BARS of the current TF
// and hard-cap so WASM stays in the tens of MB. Time-range coverage beyond
// that cap is not available from REST (a full 2000-bar 1m footprint would be
// ~1.8M prints).
const MAX_FLOW = 320000;
const TARGET_FLOW_BARS = 320;
const AGG_BURST = 16; // first extra pages in parallel for a usable first paint
const AGG_STEADY = 8; // healthy walk; drops to 2 with a 1s gap only after 429
const AGG_STEADY_MIN = 2;
const AGG_STEADY_GAP_MS = 1000;

// Market OI hist shares fapi.binance.com with aggTrades. Yield the burst so
// CLUSTER's first paint is not sitting behind 20 openInterestHist pages.
let flowWalks = 0;
export function flowWalkActive(): boolean {
  return flowWalks > 0;
}
export async function whenFlowWalkQuiet(ms = 2500): Promise<void> {
  const t0 = Date.now();
  while (flowWalks > 0 && Date.now() - t0 < ms)
    await new Promise((r) => setTimeout(r, 40));
}

export interface CandleBootstrap {
  bars: Float64Array;
  // [ts, price, qty, side] × n; side 0 = taker buy, 1 = taker sell
  flow: Float64Array;
}

// Binance futures kline intervals, in minutes
const STANDARD: [number, string][] = [
  [1, "1m"], [3, "3m"], [5, "5m"], [15, "15m"], [30, "30m"],
  [60, "1h"], [120, "2h"], [240, "4h"], [360, "6h"], [480, "8h"],
  [720, "12h"], [1440, "1d"], [4320, "3d"], [10080, "1w"], [43200, "1M"],
];

function standardString(minutes: number): string | null {
  const want = Math.round(minutes);
  for (const [m, s] of STANDARD) if (m === want) return s;
  return null;
}

// largest standard interval that evenly divides `minutes` (1m always does)
function baseInterval(minutes: number): [number, string] {
  for (let i = STANDARD.length - 1; i >= 0; i--)
    if (minutes % STANDARD[i][0] === 0) return STANDARD[i];
  return STANDARD[0];
}

let klineFapiOk = true;

async function fetchKlinesPage(
  sym: string,
  interval: string,
  endTime?: number,
): Promise<unknown[] | null> {
  const qs =
    `symbol=${sym}&interval=${interval}&limit=${KLINE_PAGE}` +
    (endTime !== undefined ? `&endTime=${endTime}` : "");
  // fapi.binance.com 418s/bans this IP under load; www.binance.com serves the
  // same klines (CORS *) and is a separate WAF bucket. Once fapi 418s, skip it
  // for the rest of the session so we don't extend the ban.
  const hosts = klineFapiOk
    ? ["https://www.binance.com", "https://fapi.binance.com"]
    : ["https://www.binance.com"];
  for (const host of hosts) {
    try {
      const res = await fetch(`${host}/fapi/v1/klines?${qs}`, {
        signal: AbortSignal.timeout(8000),
      });
      if (res.status === 418) {
        // 418 is a ban. Only drop fapi when fapi itself banned us; a www
        // 418/429 must not pin the rest of the session onto the limited host.
        if (host.includes("fapi.binance.com")) klineFapiOk = false;
        continue;
      }
      if (res.status === 429) continue;
      if (!res.ok) continue;
      const raw: unknown = await res.json();
      if (Array.isArray(raw)) return raw;
    } catch {
      /* try next host */
    }
  }
  return null;
}

// raw kline array → 7 doubles
function klineFields(k: unknown): number[] | null {
  if (!Array.isArray(k)) return null;
  return [
    Number(k[0]), Number(k[1]), Number(k[2]), Number(k[3]), Number(k[4]),
    Number(k[5]), Number(k[9]), // taker buy base volume → per-candle delta
  ];
}

function pack(rows: number[][]): Float64Array {
  const out = new Float64Array(rows.length * 7);
  for (let i = 0; i < rows.length; i++) out.set(rows[i], i * 7);
  return out;
}

function packFlow(rows: readonly AggTrade[]): Float64Array {
  const out = new Float64Array(rows.length * 4);
  for (let i = 0; i < rows.length; ++i) {
    const t = rows[i];
    out.set([t.ts, t.p, t.q, t.sell ? 1 : 0], i * 4);
  }
  return out;
}

async function fetchTimeBars(
  canon: string,
  minutes: number,
  isStale?: () => boolean,
): Promise<Float64Array | null> {
  const sym = `${canon}USDT`;
  const direct = standardString(minutes);
  if (direct) {
    const chunks: unknown[][] = [];
    let endTime: number | undefined;
    for (let page = 0; page < Math.ceil(MAX_BARS / KLINE_PAGE); ++page) {
      if (isStale?.()) return null;
      let raw = await fetchKlinesPage(sym, direct, endTime);
      if (!raw && page === 0) {
        await new Promise((r) => setTimeout(r, 800));
        if (isStale?.()) return null;
        raw = await fetchKlinesPage(sym, direct, endTime);
      }
      if (!raw || raw.length === 0) break;
      chunks.unshift(raw);
      endTime = Number((raw[0] as unknown[])[0]) - 1;
      if (raw.length < KLINE_PAGE) break;
    }
    if (chunks.length === 0) return null;
    const rows: number[][] = [];
    for (const chunk of chunks)
      for (const k of chunk) {
        const f = klineFields(k);
        if (!f) continue;
        rows.push(f);
      }
    if (rows.length === 0) return null;
    return pack(rows.slice(-MAX_BARS));
  }

  // custom interval: aggregate the largest standard base that divides it
  const [baseMin, baseStr] = baseInterval(minutes);
  const factor = minutes / baseMin;
  const pages = Math.min(Math.ceil((MAX_BARS * factor) / KLINE_PAGE), MAX_KLINE_PAGES);
  const chunks: unknown[][] = [];
  let endTime: number | undefined;
  for (let p = 0; p < pages; p++) {
    if (isStale?.()) return null;
    let raw = await fetchKlinesPage(sym, baseStr, endTime);
    if (!raw && p === 0) {
      await new Promise((r) => setTimeout(r, 800));
      if (isStale?.()) return null;
      raw = await fetchKlinesPage(sym, baseStr, endTime);
    }
    if (!raw || raw.length === 0) break;
    chunks.unshift(raw); // oldest first
    endTime = Number((raw[0] as unknown[])[0]) - 1;
    if (raw.length < KLINE_PAGE) break; // history ran out
  }
  if (chunks.length === 0) return null;

  // bucket by target-bar open time (base divides target ⇒ clean grouping)
  const targetMs = minutes * 60000;
  const bars = new Map<number, number[]>();
  const order: number[] = [];
  for (const chunk of chunks)
    for (const k of chunk) {
      const f = klineFields(k);
      if (!f) continue;
      const key = Math.floor(f[0] / targetMs);
      let b = bars.get(key);
      if (!b) {
        b = [key * targetMs, f[1], f[2], f[3], f[4], f[5], f[6]];
        bars.set(key, b);
        order.push(key);
      } else {
        b[2] = Math.max(b[2], f[2]);
        b[3] = Math.min(b[3], f[3]);
        b[4] = f[4];
        b[5] += f[5];
        b[6] += f[6];
      }
    }
  order.sort((a, b) => a - b);
  const rows = order.map((k) => bars.get(k)!);
  return pack(rows.slice(-MAX_BARS));
}

interface AggTrade {
  id: number;
  p: number;
  q: number;
  ts: number;
  sell: boolean; // m=true → buyer is maker → taker sold
}

interface AggPage {
  trades: AggTrade[];
  limited: boolean;
}

function parseAggTrades(raw: unknown): AggTrade[] {
  if (!Array.isArray(raw)) return [];
  const out: AggTrade[] = [];
  for (const t of raw) {
    if (!t || typeof t !== "object") continue;
    const o = t as Record<string, unknown>;
    out.push({
      id: Number(o.a),
      p: Number(o.p),
      q: Number(o.q),
      ts: Number(o.T),
      sell: Boolean(o.m),
    });
  }
  return out;
}

// Same WAF-bucket trick as klines: fapi hard-bans this IP under load while
// www.binance.com serves the same /fapi/v1 endpoints from a separate bucket.
// Start www-first; if www turns out unusable for aggTrades (network/!ok),
// demote it for the session so requests stop paying a dead round trip. A 418
// from fapi bans it permanently; 429 stays soft so walk pacing can back off.
let aggWwwUsable = true;
let aggFapiBanned = false;
let aggPagesFetched = 0; // diagnostic: REST pages pulled since worker boot

async function fetchAggPage(
  sym: string,
  fromId?: number,
): Promise<AggPage> {
  const qs =
    `symbol=${sym}&limit=${AGG_PAGE}` +
    (fromId !== undefined ? `&fromId=${fromId}` : "");
  const hosts: string[] = [];
  if (aggWwwUsable || aggFapiBanned) hosts.push("https://www.binance.com");
  if (!aggFapiBanned) hosts.push("https://fapi.binance.com");
  let limited = false;
  for (const host of hosts) {
    try {
      const res = await fetch(`${host}/fapi/v1/aggTrades?${qs}`, {
        signal: AbortSignal.timeout(8000),
      });
      if (res.status === 429) {
        limited = true;
        continue;
      }
      if (res.status === 418) {
        if (host !== "https://www.binance.com") aggFapiBanned = true;
        limited = true;
        continue;
      }
      if (!res.ok) {
        if (host === "https://www.binance.com") aggWwwUsable = false;
        continue;
      }
      const trades = parseAggTrades(await res.json());
      if (trades.length) ++aggPagesFetched;
      return { trades, limited: false };
    } catch {
      if (host === "https://www.binance.com") aggWwwUsable = false;
      /* try next host */
    }
  }
  return { trades: [], limited };
}

async function fetchAggPageRetry(
  sym: string,
  fromId?: number,
): Promise<AggTrade[] | null> {
  for (let attempt = 0; attempt < 4; attempt++) {
    const page = await fetchAggPage(sym, fromId);
    if (page.trades.length) return page.trades;
    // A 200 with an empty body at a historical fromId is the end of the
    // book, not a blip — retrying it 6× used to stall the walk for seconds.
    if (!page.limited && fromId !== undefined) return page.trades;
    await new Promise((resolve) =>
      setTimeout(resolve, page.limited ? 800 : 280 * (attempt + 1)),
    );
  }
  return null;
}

function mergeById(a: AggTrade[], b: AggTrade[]): AggTrade[] {
  if (b.length === 0) return a;
  if (a.length === 0) return b;
  const out: AggTrade[] = [];
  let i = 0, j = 0;
  while (i < a.length && j < b.length) {
    if (a[i].id === b[j].id) {
      out.push(a[i]);
      i++;
      j++;
    } else if (a[i].id < b[j].id) {
      out.push(a[i++]);
    } else {
      out.push(b[j++]);
    }
  }
  while (i < a.length) out.push(a[i++]);
  while (j < b.length) out.push(b[j++]);
  return out;
}

function clampNeed(need: number): number {
  return Math.min(MAX_FLOW, Math.max(AGG_PAGE, Math.ceil(need)));
}

// ---------------------------------------------------------------------------
// Raw-print window cache
//
// Walked aggressor-print windows are timeframe-independent — only how far
// back a chart needs them differs. Bank the newest window per symbol (LRU)
// so a TF switch repaints instantly from the cached window and only walks
// older pages when the new TF genuinely needs more history. Walks bank their
// progress as they go, so cancelled/restarted walks resume where they left
// off instead of re-pulling hundreds of REST pages from scratch.
// ---------------------------------------------------------------------------
const flowCache = new Map<string, AggTrade[]>(); // canon → ascending-id window
const FLOW_CACHE_SYMS = 2;

function flowCacheGet(canon: string): AggTrade[] | null {
  const hit = flowCache.get(canon);
  if (!hit || hit.length === 0) return null;
  flowCache.delete(canon); // LRU touch
  flowCache.set(canon, hit);
  return hit;
}

function flowCachePut(canon: string, trades: AggTrade[]): void {
  const trimmed =
    trades.length > MAX_FLOW ? trades.slice(trades.length - MAX_FLOW) : trades;
  flowCache.delete(canon);
  flowCache.set(canon, trimmed);
  while (flowCache.size > FLOW_CACHE_SYMS)
    flowCache.delete(flowCache.keys().next().value!);
}

// Bridge the id-gap between a cached window and a fresh newest page — idle
// time leaves one behind, and ids are contiguous per symbol so the gap is
// exactly known. Speculative parallel pages are safe: fromId=X covers ids
// X..X+999 verbatim. Capped so a very long idle cannot stall the repaint;
// walkAggTrades' fillHoles patches any small residue past the cap.
const BRIDGE_PAGE_CAP = 120;

async function bridgeWindowGap(
  sym: string,
  cached: AggTrade[],
  latest: AggTrade[],
): Promise<AggTrade[]> {
  const from = cached[cached.length - 1].id + 1;
  const upto = latest[0].id; // exclusive
  if (upto <= from) return mergeById(cached, latest);
  const merged = cached.slice();
  const pages = Math.min(Math.ceil((upto - from) / AGG_PAGE), BRIDGE_PAGE_CAP);
  for (let i = 0; i < pages; i += AGG_STEADY) {
    const starts: number[] = [];
    for (let p = i; p < i + AGG_STEADY && p < pages; ++p)
      starts.push(from + p * AGG_PAGE);
    const got = await Promise.all(starts.map((id) => fetchAggPage(sym, id)));
    let any = false;
    for (const page of got)
      for (const t of page.trades)
        if (t.id < upto) {
          merged.push(t);
          any = true;
        }
    if (!any && got.every((p) => !p.limited)) break; // history ended
  }
  return mergeById(merged, latest); // dedupes the seam overlap
}

// How many aggTrades it takes to cover ~TARGET_FLOW_BARS of this timeframe,
// estimated from the latest page's density.
function flowNeed(sample: AggTrade[], kind: number, value: number): number {
  const span = Math.max(1, sample[sample.length - 1].ts - sample[0].ts);
  const perMs = sample.length / span;
  if (kind === TF_TICK)
    return clampNeed(Math.min(MAX_BARS, TARGET_FLOW_BARS) * value);
  if (kind === TF_VOLUME) {
    let q = 0;
    for (const t of sample) q += t.q;
    const avgQ = q / sample.length;
    if (avgQ > 0) return clampNeed((TARGET_FLOW_BARS * value) / avgQ);
  }
  const minutes = Math.max(1, value);
  return clampNeed(perMs * TARGET_FLOW_BARS * minutes * 60000);
}

// Called as older fromId pages arrive. Return false to cancel the walk.
export type FlowSink = (chunk: Float64Array, prepend: boolean) => boolean;

function tfLabel(kind: number, value: number): string {
  return kind === TF_TICK ? `${value}t` : kind === TF_VOLUME ? `${value}v` : `${value}m`;
}

// Walk telemetry for probes/devtools: page console via the bridge diag
// channel (worker consoles are separate worlds headless-side).
function diag(text: string): void {
  console.log(text);
  (self as unknown as Worker).postMessage({ kind: "diag", text });
}

// Walk aggTrade ids oldest-ward from `seed`. IDs are contiguous per symbol,
// so fromId pages are disjoint. Burst + fillHoles repair 429 gaps, then the
// steady walk always pages from the oldest id we hold. `sink` gets a window
// replace first (need-capped tail of the seed) then older slices as they
// arrive, so CLUSTER paints live; `onTick` gets the full working window
// after each growth step so bar bucketing can repaint progressively.
// Progress banks into the per-symbol flow cache under `canon` (null skips).
// `isStale` cancels superseded walks at wave boundaries — checked before new
// REST work starts, never mid-wave, so in-flight pages always complete.
async function walkAggTrades(
  sym: string,
  canon: string | null,
  seed: AggTrade[],
  need: number,
  sink?: FlowSink,
  onTick?: (trades: AggTrade[]) => void,
  isStale?: () => boolean,
): Promise<AggTrade[]> {
  flowWalks++;
  try {
    return await walkAggTradesInner(sym, canon, seed, need, sink, onTick, isStale);
  } finally {
    flowWalks--;
  }
}

async function fetchAggPages(
  sym: string,
  starts: number[],
): Promise<{ trades: AggTrade[]; limited: boolean }> {
  if (starts.length === 0) return { trades: [], limited: false };
  const first = await Promise.all(starts.map((id) => fetchAggPage(sym, id)));
  let limited = first.some((p) => p.limited);
  const byStart = new Map<number, AggTrade[]>();
  for (let i = 0; i < starts.length; i++) byStart.set(starts[i], first[i].trades);
  const missing = starts.filter((id, i) => first[i].limited);
  if (missing.length) {
    await new Promise((r) => setTimeout(r, 400));
    const again = await Promise.all(missing.map((id) => fetchAggPage(sym, id)));
    for (let i = 0; i < missing.length; i++) {
      if (again[i].limited) limited = true;
      if (again[i].trades.length) byStart.set(missing[i], again[i].trades);
    }
  }
  const batch: AggTrade[] = [];
  const seen = new Set<number>();
  for (const pg of byStart.values()) {
    for (const t of pg) {
      if (!seen.has(t.id)) {
        seen.add(t.id);
        batch.push(t);
      }
    }
  }
  batch.sort((a, b) => a.id - b.id);
  return { trades: batch, limited };
}

async function walkAggTradesInner(
  sym: string,
  canon: string | null,
  seed: AggTrade[],
  need: number,
  sink?: FlowSink,
  onTick?: (trades: AggTrade[]) => void,
  isStale?: () => boolean,
): Promise<AggTrade[]> {
  need = clampNeed(need);
  let trades = seed;
  const bank = () => {
    if (canon) flowCachePut(canon, trades);
  };
  // Replace payloads are capped at `need`: a cache-seeded window can be far
  // deeper than the current TF asks for, and shipping all of it would make
  // wasm reload megabytes of prints the chart will not render.
  const packTail = (): Float64Array =>
    trades.length <= need ? packFlow(trades) : packFlow(trades.slice(trades.length - need));

  // Paint the newest page immediately so CLUSTER prefill is not empty
  // while the burst is in flight.
  if (sink && !sink(packTail(), false)) return trades;
  if (onTick) onTick(trades);
  bank();
  if (trades.length >= need) return trades.slice(-need);

  const pullOlder = async (
    pages: number,
  ): Promise<{ older: AggTrade[]; limited: boolean }> => {
    const oldestId = trades[0].id;
    const starts: number[] = [];
    for (let i = 1; i <= pages; i++) {
      const from = oldestId - i * AGG_PAGE;
      if (from < 0) break;
      starts.push(from);
    }
    if (starts.length === 0) return { older: [], limited: false };
    const fetched = await fetchAggPages(sym, starts);
    const older = fetched.trades.filter((t) => t.id < oldestId);
    if (older.length === 0) return { older: [], limited: fetched.limited };
    trades = mergeById(older, trades);
    if (trades.length > need) trades = trades.slice(trades.length - need);
    const keepOldest = trades[0].id;
    return {
      older: older.filter((t) => t.id >= keepOldest),
      limited: fetched.limited,
    };
  };

  const fillHoles = async (): Promise<boolean> => {
    let patched = false;
    for (let guard = 0; guard < 6; guard++) {
      const holes: number[] = [];
      for (let i = 1; i < trades.length && holes.length < 16; i++) {
        const gap = trades[i].id - trades[i - 1].id;
        if (gap <= 50) continue;
        for (
          let id = trades[i - 1].id + 1;
          id < trades[i].id && holes.length < 16;
          id += AGG_PAGE
        )
          holes.push(id);
      }
      if (holes.length === 0) return patched;
      const fetched = await fetchAggPages(sym, holes);
      if (fetched.trades.length === 0) return patched;
      const before = trades.length;
      trades = mergeById(trades, fetched.trades);
      if (trades.length > need) trades = trades.slice(trades.length - need);
      if (trades.length === before) return patched;
      patched = true;
    }
    return patched;
  };

  const burst = await pullOlder(AGG_BURST);
  await fillHoles();
  bank();
  if (onTick) onTick(trades);
  if (isStale && isStale()) return trades;
  if (sink && trades.length && !sink(packTail(), false)) return trades;

  // Steady walk goes left from the oldest id we hold. Hole repair is
  // fillHoles' job (once after the burst, once at the end) so a 429 cannot
  // redirect every wave into already-held ids. Prepend older slices instead
  // of replacing the whole window — wasm was rebuilding CLUSTER from 100k+
  // prints on every pair of pages.
  let conc = burst.limited ? AGG_STEADY_MIN : AGG_STEADY;
  let delay = burst.limited ? AGG_STEADY_GAP_MS : 0;
  let pending: AggTrade[] = [];
  const flushPending = (): boolean => {
    if (!sink || pending.length === 0) return true;
    const packed = packFlow(pending);
    pending = [];
    return sink(packed, true);
  };
  let emptyLimited = 0;
  while (trades.length < need) {
    if (isStale && isStale()) break;
    if (delay) await new Promise((resolve) => setTimeout(resolve, delay));
    const wave = await pullOlder(conc);
    if (wave.older.length === 0) {
      if (!wave.limited || ++emptyLimited > 8) break;
    } else {
      emptyLimited = 0;
    }
    if (wave.limited) {
      conc = AGG_STEADY_MIN;
      delay = AGG_STEADY_GAP_MS;
    } else {
      conc = Math.min(AGG_STEADY, conc + 2);
      delay = 0;
    }
    if (wave.older.length) {
      pending = wave.older.concat(pending);
      bank();
      if (onTick) onTick(trades);
    }
    if (pending.length >= 8000 && !flushPending()) return trades;
  }
  bank();
  if (!flushPending()) return trades;
  if (isStale && isStale()) return trades;
  const endHoles = await fillHoles();
  if (endHoles) {
    bank();
    if (onTick) onTick(trades);
    if (sink && trades.length && !sink(packTail(), false)) return trades;
  }
  return trades.slice(-need);
}

// Progressive tick/volume bootstrap: called with fully-bucketed bars (and
// the packed flow window when requested) after the burst and every steady
// wave, so tick/volume charts paint while older pages are still in flight.
export type BarsProgress = (bars: Float64Array, flow: Float64Array) => void;

// Tick/volume bar history: aggTrade ids are contiguous per symbol, so after
// one latest-page request the remaining history pages walk back by fromId.
// Capped at MAX_FLOW so the REST pull cannot run unbounded. Seeded from the
// per-symbol raw-window cache: a warm cache repaints instantly and only
// walks whatever older prefix the request actually needs.
async function fetchAggBars(
  canon: string,
  kind: number,
  value: number,
  includeFlow: boolean,
  onProgress?: BarsProgress,
  isStale?: () => boolean,
): Promise<CandleBootstrap | null> {
  const sym = `${canon}USDT`;
  const latest = await fetchAggPageRetry(sym);
  if (!latest || latest.length === 0) return null;
  const cached = flowCacheGet(canon);
  const seed = cached ? await bridgeWindowGap(sym, cached, latest) : latest;
  flowCachePut(canon, seed);

  let need = MAX_FLOW;
  if (kind === TF_TICK) need = clampNeed(MAX_BARS * value);
  if (kind === TF_VOLUME) {
    let q = 0;
    for (const t of latest) q += t.q;
    const avgQ = q / latest.length;
    if (avgQ > 0) need = clampNeed((MAX_BARS * value) / avgQ);
  }
  if (includeFlow) need = Math.max(need, flowNeed(latest, kind, value));

  const bucket = (trades: readonly AggTrade[]): Float64Array => {
    // bucket forward, oldest → newest; the final (possibly partial) bar is
    // kept so the chart shows the very latest trades — the live bucketer
    // treats it as closed for tick (count unknown) and continues it for volume
    const rows: number[][] = [];
    let cur: number[] | null = null;
    let count = 0;
    for (const t of trades) {
      if (!cur || (kind === TF_TICK ? count >= value : cur[5] >= value)) {
        cur = [t.ts, t.p, t.p, t.p, t.p, 0, 0];
        rows.push(cur);
        count = 0;
      }
      if (t.p > cur![2]) cur![2] = t.p;
      if (t.p < cur![3]) cur![3] = t.p;
      cur![4] = t.p;
      cur![5] += t.q;
      if (!t.sell) cur![6] += t.q; // taker buy
      count++;
    }
    return pack(rows.slice(-MAX_BARS));
  };

  let posted = 0;
  const t0 = Date.now();
  const pages0 = aggPagesFetched;
  const all = await walkAggTrades(
    sym,
    canon,
    seed,
    need,
    undefined,
    onProgress
      ? (window) => {
          // throttle to ~one post per wave of new trades
          if (window.length < need && window.length - posted < 8000) return;
          posted = window.length;
          onProgress(
            bucket(window),
            includeFlow ? packFlow(window) : new Float64Array(),
          );
        }
      : undefined,
    isStale,
  );
  flowCachePut(canon, all);
  if (all.length === 0) return null;
  diag(
    `[bars] ${canon} ${tfLabel(kind, value)} need=${need} seed=${seed.length}` +
    ` pages=${aggPagesFetched - pages0} in ${Date.now() - t0}ms` +
    (isStale && isStale() ? " (cancelled)" : ""),
  );
  return {
    bars: bucket(all),
    flow: includeFlow ? packFlow(all) : new Float64Array(),
  };
}

export async function fetchCandles(
  canon: string,
  kind: number,
  value: number,
  includeFlow = false,
  onProgress?: BarsProgress,
  isStale?: () => boolean,
): Promise<CandleBootstrap | null> {
  if (kind === TF_TIME) {
    // Time OHLC comes from klines. Footprints are a separate aggTrade pull
    // (fetchOrderFlow) so a TF switch can paint candles immediately instead of
    // blocking on hundreds of weight-20 pages.
    const bars = await fetchTimeBars(canon, value, isStale);
    if (!bars) return null;
    return { bars, flow: new Float64Array() };
  }
  return fetchAggBars(canon, kind, value, includeFlow, onProgress, isStale);
}

// Footprint bootstrap only — does not touch the kline/OHLC series. Switching
// chart type must not re-post candles (that wiped heatmap columns and the
// live forming bar). `sink` is called with a need-capped window as a replace
// (instant when the raw-print cache already covers this TF), then with older
// slices only while the walk extends past what was banked.
export async function fetchOrderFlow(
  canon: string,
  kind: number,
  value: number,
  sink: FlowSink,
): Promise<number> {
  const sym = `${canon}USDT`;
  const t0 = Date.now();
  const pages0 = aggPagesFetched;
  // Fresh newest page first: density sample for sizing the walk, plus the
  // seam the bridge stitches any idle-gap up to.
  const latest = await fetchAggPageRetry(sym);
  if (!latest || latest.length === 0) return 0;
  const cached = flowCacheGet(canon);
  const seed = cached ? await bridgeWindowGap(sym, cached, latest) : latest;
  const need = flowNeed(latest, kind, value);
  flowCachePut(canon, seed); // bank before walking so cancels keep progress
  const all = await walkAggTrades(sym, canon, seed, need, sink);
  diag(
    `[flow] ${canon} ${tfLabel(kind, value)} need=${clampNeed(need)}` +
    ` seed=${seed.length} pages=${aggPagesFetched - pages0} got=${all.length}` +
    ` in ${Date.now() - t0}ms`,
  );
  return all.length;
}
