// Binance USD-M candle history for the chart panel.
// Posted to the main thread as transferable packed arrays: OHLCV bars plus,
// when requested by a footprint chart, exact [ts, price, qty, aggressor side]
// aggTrades. Ordinary candle charts do not pay the history/network cost.
//
// Time bars come from the klines API: standard intervals directly, custom
// minute counts by aggregating the largest standard interval that evenly
// divides the target (paged backwards so the 2000-bar chart cap still fills).
// Tick/volume bars can't come from klines — they are bootstrapped from the
// aggTrades endpoint, paged backwards by aggregate trade ID.

export const TF_TIME = 0;
export const TF_TICK = 1;
export const TF_VOLUME = 2;

const MAX_BARS = 2000; // enough for a complete 1m UTC TPO session
const KLINE_PAGE = 1000;
const AGG_PAGE = 1000;
const AGG_HISTORY_MS = 48 * 3600000;
// Exact aggressor prints for CLUSTER/PROFILE. Klines still load MAX_BARS, but
// Binance only serves aggTrades 1000-at-a-time (weight 20). A flat 80k window
// is ~90 1m bars / ~17 5m bars on a busy BTC tape, so a zoomed CLUSTER pane
// was half empty OHLC. Size the pull to ~TARGET_FLOW_BARS of the current TF
// and hard-cap so WASM stays in the tens of MB. Time-range coverage beyond
// that cap is outside the retained window. REST also limits queries to 48h.
const MAX_FLOW = 320000;
const TARGET_FLOW_BARS = 320;
// Leave request-weight headroom for candles and market metadata. One request
// at a time also prevents an interrupted page from leaving holes in history.
const AGG_REQUEST_GAP_MS = 650;
let aggNextRequestAt = 0;
let aggRequestTail: Promise<unknown> = Promise.resolve();

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

let klineRetryAt = 0;
export function candleRetryDelay(): number {
  return Math.max(0, klineRetryAt - Date.now());
}

async function fetchKlinesPage(
  sym: string,
  interval: string,
  endTime?: number,
): Promise<unknown[] | null> {
  const qs =
    `symbol=${sym}&interval=${interval}&limit=${KLINE_PAGE}` +
    (endTime !== undefined ? `&endTime=${endTime}` : "");
  if (candleRetryDelay() > 0) return null;
  const hosts = ["https://www.binance.com", "https://fapi.binance.com"];
  for (const host of hosts) {
    try {
      const res = await fetch(`${host}/fapi/v1/klines?${qs}`, {
        signal: AbortSignal.timeout(8000),
      });
      if (res.status === 418 || res.status === 429) {
        const retry = res.headers?.get("retry-after");
        const seconds = Number(retry);
        let until = retry ? (Number.isFinite(seconds) ? Date.now() + seconds * 1000 : Date.parse(retry)) : 0;
        try {
          const error = await res.json();
          const match = String(error?.msg || "").match(/banned until (\d+)/i);
          if (match) until = Math.max(until || 0, Number(match[1]));
        } catch { /* fallback cooldown below */ }
        klineRetryAt = Math.max(klineRetryAt, Number.isFinite(until) ? until : 0,
          Date.now() + (res.status === 418 ? 60000 : 5000));
        // These hosts can share the IP limit. Do not retry another host during a ban.
        return null;
      }
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

interface TimeWalk {
  rows: Map<number, number[]>;
  oldest: number;
  endTime: number | undefined;
}
const timeWalks = new Map<string, TimeWalk>();

async function fetchTimeBars(
  canon: string,
  minutes: number,
  isStale?: () => boolean,
  onProgress?: BarsProgress,
): Promise<Float64Array | null> {
  const sym = `${canon}USDT`;
  const direct = standardString(minutes);
  if (direct) {
    const chunks: unknown[][] = [];
    let endTime: number | undefined;
    for (let page = 0; page < Math.ceil(MAX_BARS / KLINE_PAGE); ++page) {
      if (isStale?.()) return null;
      let raw = await fetchKlinesPage(sym, direct, endTime);
      if (!raw && !candleRetryDelay()) {
        await new Promise((r) => setTimeout(r, 800));
        if (isStale?.()) return null;
        raw = await fetchKlinesPage(sym, direct, endTime);
      }
      if (!raw) return null;
      if (raw.length === 0) break;
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

  // Keep only target buckets, not tens of thousands of source candles.
  // Failed walks retain their cursor so retries do not download the newest pages again.
  const key = `${sym}:${minutes}`;
  let walk = timeWalks.get(key);
  if (!walk) {
    walk = { rows: new Map(), oldest: Infinity, endTime: undefined };
    timeWalks.set(key, walk);
    while (timeWalks.size > 3) timeWalks.delete(timeWalks.keys().next().value!);
  }
  const cancelled = () => {
    if (!isStale?.()) return false;
    if (timeWalks.get(key) === walk) timeWalks.delete(key);
    return true;
  };
  const targetMs = minutes * 60000;
  const [, baseStr] = baseInterval(minutes);
  const snapshot = () => {
    const rows = [...walk.rows.values()].sort((a,b) => a[0]-b[0]);
    if (rows.length && walk.oldest > rows[0][0]) rows.shift();
    return pack(rows.slice(-MAX_BARS));
  };
  if (walk.rows.size) onProgress?.(snapshot(), new Float64Array());
  for (let page = 0; ; ++page) {
    if (cancelled()) return null;
    let raw = await fetchKlinesPage(sym, baseStr, walk.endTime);
    for (let retry = 0; !raw && !candleRetryDelay() && retry < 2; ++retry) {
      await new Promise((r) => setTimeout(r, 800 * (retry + 1)));
      if (cancelled()) return null;
      raw = await fetchKlinesPage(sym, baseStr, walk.endTime);
    }
    if (cancelled()) return null;
    if (!raw) return null; // incomplete history must be retried, not marked successful
    if (!raw.length) { timeWalks.delete(key); return snapshot(); }
    const first = Number((raw[0] as unknown[])[0]);
    if (!Number.isFinite(first) || (walk.endTime !== undefined && first > walk.endTime)) return null;
    const older = new Map<number, number[]>();
    for (const k of raw) {
      const f = klineFields(k);
      if (!f || !f.every(Number.isFinite)) continue;
      const ts = Math.floor(f[0] / targetMs) * targetMs;
      const row = older.get(ts);
      if (!row) older.set(ts, [ts, ...f.slice(1)]);
      else { row[2]=Math.max(row[2],f[2]); row[3]=Math.min(row[3],f[3]); row[4]=f[4]; row[5]+=f[5]; row[6]+=f[6]; }
    }
    for (const [ts, row] of older) {
      const newer = walk.rows.get(ts);
      if (newer) { row[2]=Math.max(row[2],newer[2]); row[3]=Math.min(row[3],newer[3]); row[4]=newer[4]; row[5]+=newer[5]; row[6]+=newer[6]; }
      walk.rows.set(ts,row);
    }
    walk.oldest=first; walk.endTime=first-1;
    const result=snapshot();
    if (result.length / 7 >= MAX_BARS || raw.length < KLINE_PAGE) {
      timeWalks.delete(key); return result;
    }
    if (page === 0 || page % 4 === 0) onProgress?.(result,new Float64Array());
    await new Promise((r) => setTimeout(r, 120));
  }
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
}

function parseAggTrades(raw: unknown): AggTrade[] {
  if (!Array.isArray(raw)) return [];
  const out: AggTrade[] = [];
  for (const t of raw) {
    if (!t || typeof t !== "object") continue;
    const o = t as Record<string, unknown>;
    if (!Number.isSafeInteger(Number(o.a)) || Number(o.a) < 0 ||
        !Number.isFinite(Number(o.p)) || !(Number(o.p) > 0) ||
        !Number.isFinite(Number(o.q)) || !(Number(o.q) > 0) ||
        !Number.isFinite(Number(o.T)) || !(Number(o.T) > 0) || typeof o.m !== "boolean") continue;
    out.push({
      id: Number(o.a),
      p: Number(o.p),
      q: Number(o.q),
      ts: Number(o.T),
      sell: Boolean(o.m),
    });
  }
  out.sort((a,b) => a.id-b.id);
  return out;
}

let aggPagesFetched = 0;

async function fetchAggPage(sym: string, fromId?: number, isStale?: () => boolean): Promise<AggPage> {
  const request = aggRequestTail.then(async () => {
    if (isStale?.()) throw new Error("Superseded trade history request");
    if (candleRetryDelay() > 0) throw new Error("Trade history cooling down");
    const wait = Math.max(0, aggNextRequestAt - Date.now());
    if (wait) await new Promise(r => setTimeout(r, wait));
    if (isStale?.()) throw new Error("Superseded trade history request");
    if (candleRetryDelay() > 0) throw new Error("Trade history cooling down");
    aggNextRequestAt = Date.now() + AGG_REQUEST_GAP_MS;
    const qs = `symbol=${sym}&limit=${AGG_PAGE}` +
      (fromId !== undefined ? `&fromId=${fromId}` : "");
    const res = await fetch(`https://fapi.binance.com/fapi/v1/aggTrades?${qs}`, {
      signal: AbortSignal.timeout(8000),
    });
    if (res.status === 418 || res.status === 429) {
      const retry = res.headers?.get("retry-after");
      const seconds = Number(retry);
      let until = retry ? (Number.isFinite(seconds) ? Date.now() + seconds * 1000 : Date.parse(retry)) : 0;
      try {
        const error = await res.json();
        const match = String(error?.msg || "").match(/banned until (\d+)/i);
        if (match) until = Math.max(until || 0, Number(match[1]));
      } catch { /* fallback cooldown */ }
      klineRetryAt = Math.max(klineRetryAt, Number.isFinite(until) ? until : 0,
        Date.now() + (res.status === 418 ? 60000 : 5000));
      throw new Error("Trade history rate limited");
    }
    if (!res.ok) throw new Error(`Trade history HTTP ${res.status}`);
    const raw: unknown = await res.json();
    if (!Array.isArray(raw)) throw new Error("Invalid trade history response");
    const trades = parseAggTrades(raw);
    if (trades.length !== raw.length || trades.some((t,i) => i > 0 && t.id !== trades[i-1].id+1)) throw new Error("Invalid trade history row");
    ++aggPagesFetched;
    return {trades};
  });
  aggRequestTail = request.catch(() => {});
  return request;
}

async function fetchAggPageRetry(sym: string, fromId?: number, isStale?: () => boolean): Promise<AggTrade[] | null> {
  return (await fetchAggPage(sym, fromId, isStale)).trades;
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

// Only seed from overlapping/adjacent cached ranges. Combining disconnected
// windows falsely counted the missing IDs toward completion. A new contiguous
// tail banks progress immediately and subsequent retries resume that tail.
async function bridgeWindowGap(sym: string, cached: AggTrade[], latest: AggTrade[]): Promise<AggTrade[]> {
  return latest[0].id <= cached[cached.length - 1].id + 1
    ? mergeById(cached, latest) : latest;
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
// so fromId pages are disjoint. Only contiguous completed pages advance
// the cached cursor; errors leave the missing page pending for a retry. `sink` gets a window
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
  let trades = seed.slice(-need);
  const bank = () => { if (canon) flowCachePut(canon, trades); };
  if (isStale?.()) return trades;
  bank();
  if (sink && !sink(packFlow(trades), false)) return trades;
  onTick?.(trades);
  let pending: AggTrade[] = [];
  const flush = (): boolean => {
    if (!pending.length) return true;
    const chunk = pending; pending = [];
    onTick?.(trades);
    return !sink || sink(packFlow(chunk), true);
  };
  try {
    while (trades.length < need && trades[0].id > 0) {
      if (isStale?.()) return trades;
      if (trades[0].ts <= Date.now() - AGG_HISTORY_MS) break;
      const oldest = trades[0].id;
      const from = Math.max(0, oldest - AGG_PAGE);
      const page = (await fetchAggPage(sym, from, isStale)).trades;
      if (isStale?.()) return trades;
      if (!page.length) break; // Only a successful empty response is EOF.
      const older = page.filter(t => t.id >= from && t.id < oldest);
      const atRetentionBoundary = older.length > 0 && older[0].id > from &&
        older[0].ts <= Date.now() - AGG_HISTORY_MS + 60000;
      if (!older.length || (older[0].id !== from && !atRetentionBoundary) || older.at(-1)!.id !== oldest - 1 ||
          older.some((t,i) => i > 0 && t.id !== older[i-1].id + 1))
        throw new Error("Incomplete trade history page");
      const kept = older.slice(-Math.min(older.length, need-trades.length));
      trades = kept.concat(trades);
      pending = kept.concat(pending);
      bank();
      if (pending.length >= 4000 && !flush()) return trades;
      if (atRetentionBoundary) break;
    }
  } finally {
    // Publish completed pages even if the next page fails. Retrying resumes
    // the contiguous cached prefix rather than restarting from the latest page.
    if (!isStale?.()) flush();
  }
  return trades;
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
  if (isStale?.()) return null;
  const latest = await fetchAggPageRetry(sym, undefined, isStale);
  if (isStale?.() || !latest || latest.length === 0) return null;
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
          if (posted > 0 && window.length < need && window.length - posted < 4000) return;
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
    const bars = await fetchTimeBars(canon, value, isStale, onProgress);
    if (!bars) return null;
    return { bars, flow: new Float64Array() };
  }
  try { return await fetchAggBars(canon, kind, value, includeFlow, onProgress, isStale); }
  catch { return null; } // worker resumes banked progress after bounded backoff
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
  isStale?: () => boolean,
): Promise<number> {
  const sym = `${canon}USDT`;
  const t0 = Date.now();
  const pages0 = aggPagesFetched;
  const banked = flowCacheGet(canon);
  if (banked && !isStale?.() && !sink(packFlow(banked), false)) return banked.length;
  // Fresh newest page first: density sample for sizing the walk, plus the
  // seam the bridge stitches any idle-gap up to.
  if (isStale?.()) return 0;
  const latest = await fetchAggPageRetry(sym, undefined, isStale);
  if (isStale?.()) return 0;
  if (!latest || latest.length === 0) return 0;
  const cached = flowCacheGet(canon);
  const seed = cached ? await bridgeWindowGap(sym, cached, latest) : latest;
  const need = kind === TF_TIME ? MAX_FLOW : flowNeed(latest, kind, value);
  flowCachePut(canon, seed); // bank before walking so cancels keep progress
  let initial = true;
  const alreadyPainted = banked && seed.length === banked.length &&
    seed[0].id === banked[0].id && seed.at(-1)!.id === banked.at(-1)!.id && seed.length <= need;
  const all = await walkAggTrades(sym, canon, seed, need, (chunk, prepend) => {
    if (initial) { initial = false; if (alreadyPainted) return true; }
    return sink(chunk, prepend);
  }, undefined, isStale);
  diag(
    `[flow] ${canon} ${tfLabel(kind, value)} need=${clampNeed(need)}` +
    ` seed=${seed.length} pages=${aggPagesFetched - pages0} got=${all.length}` +
    ` in ${Date.now() - t0}ms`,
  );
  return all.length;
}

// Calendar opens do not depend on whether a custom interval hits midnight.
// Cache per symbol so switching chart timeframe does not refetch daily history.
const calendarCache = new Map<string, { at: number; pending: Promise<Float64Array> }>();
export function fetchCalendarOpens(canon: string): Promise<Float64Array> {
  const cached = calendarCache.get(canon);
  if (cached && Date.now() - cached.at < 300000 &&
      Math.floor(cached.at / 86400000) === Math.floor(Date.now() / 86400000)) return cached.pending;
  const pending = fetchKlinesPage(`${canon}USDT`, "1d").then(raw => {
    const rows: number[] = [];
    for (const k of raw || []) {
      const f = klineFields(k);
      if (f && f[0] % 86400000 === 0) rows.push(f[0], f[1]);
    }
    if (!rows.length) calendarCache.delete(canon);
    return new Float64Array(rows);
  });
  calendarCache.set(canon, {at: Date.now(), pending});
  return pending;
}
