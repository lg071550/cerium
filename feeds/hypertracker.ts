// HyperTracker (CoinMarketMan) client for live HL liquidation / stop-loss
// profiles. Free tier is 100 REST calls / UTC day. Each layer is one download;
// the binned snapshot lives in IndexedDB and is refreshed on HT_LIVE_MS so a
// reload does not spend quota on a book we just fetched.

import { SYMBOLS } from "./registry";

export const HT_QUOTA = 100;
export const HT_SOFT_CAP = 80; // stop proactive refresh; still fill a cold cache
export const HT_LIVE_MS = 15 * 60 * 1000;

export const HT_OK = 0;
export const HT_NO_TOKEN = 1;
export const HT_QUOTA_HIT = 2;
export const HT_ERROR = 3;

export interface HtBand {
  lo: number;
  hi: number;
  longUsd: number;
  shortUsd: number;
}

export interface HtLayer {
  bands: HtBand[];
  fetchedAt: number;
  ref: number;
}

export interface HtState {
  coin: string;
  sym: number;
  status: number;
  used: number;
  quota: number;
  liq: HtLayer | null;
  sl: HtLayer | null;
}

export interface HtConfig {
  token: string;
  sym: number;
  liq: boolean;
  sl: boolean;
}

type CacheKind = "liq" | "sl";

interface QuotaRec {
  day: string;
  used: number;
}

interface LayerRec {
  coin: string;
  kind: CacheKind;
  fetchedAt: number;
  ref: number;
  bands: HtBand[];
}

const DB_NAME = "cerium-ht";
const STORE = "kv";
const PROXY = "/ht";

let dbPromise: Promise<IDBDatabase> | null = null;
let timer: ReturnType<typeof setTimeout> | null = null;
let cfg: HtConfig = { token: "", sym: 0, liq: false, sl: false };
let inflight: Promise<void> | null = null;
let lastPosted = "";
let configVersion = 0;

function utcDay(ms = Date.now()): string {
  return new Date(ms).toISOString().slice(0, 10);
}

function coinOf(sym: number): string {
  return SYMBOLS[sym] ?? "ETH";
}

function openDb(): Promise<IDBDatabase> {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      if (!req.result.objectStoreNames.contains(STORE))
        req.result.createObjectStore(STORE);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
  return dbPromise;
}

function idbGet<T>(key: string): Promise<T | undefined> {
  return openDb().then(
    (db) =>
      new Promise((resolve, reject) => {
        const r = db.transaction(STORE, "readonly").objectStore(STORE).get(key);
        r.onsuccess = () => resolve(r.result as T | undefined);
        r.onerror = () => reject(r.error);
      }),
  );
}

function idbSet(key: string, value: unknown): Promise<void> {
  return openDb().then(
    (db) =>
      new Promise((resolve, reject) => {
        const r = db.transaction(STORE, "readwrite").objectStore(STORE).put(value, key);
        r.onsuccess = () => resolve();
        r.onerror = () => reject(r.error);
      }),
  );
}

async function loadQuota(): Promise<QuotaRec> {
  const day = utcDay();
  const rec = await idbGet<QuotaRec>("quota");
  if (!rec || rec.day !== day) return { day, used: 0 };
  return rec;
}

async function bumpQuota(): Promise<QuotaRec> {
  const rec = await loadQuota();
  rec.used += 1;
  await idbSet("quota", rec);
  return rec;
}

function layerKey(kind: CacheKind, coin: string): string {
  return `${kind}:${coin}:v2`;
}

function niceBin(mid: number): number {
  const raw = Math.max(mid * 0.0008, 1e-8);
  const exp = Math.pow(10, Math.floor(Math.log10(raw)));
  const n = raw / exp;
  const step = n < 1.5 ? 1 : n < 3.5 ? 2 : 5;
  return step * exp;
}

function percentile(values: number[], p: number): number {
  if (values.length === 0) return 1;
  const s = values.slice().sort((a, b) => a - b);
  const i = Math.min(s.length - 1, Math.max(0, Math.floor((s.length - 1) * p)));
  return s[i]! > 0 ? s[i]! : 1;
}

function binPoints(
  points: { px: number; usd: number; longSide: boolean }[],
  binArg?: number,
): HtLayer {
  const now = Date.now();
  if (points.length === 0) return { bands: [], fetchedAt: now, ref: 1 };
  const prices = points.map((p) => p.px).sort((a, b) => a - b);
  const mid = prices[(prices.length / 2) | 0]!;
  const bin = binArg && binArg > 0 ? binArg : niceBin(mid);
  const loBound = mid * 0.35;
  const hiBound = mid * 1.85;
  const map = new Map<number, { longUsd: number; shortUsd: number }>();
  for (const p of points) {
    if (!(p.px > 0) || !(p.usd > 0) || !Number.isFinite(p.px) || !Number.isFinite(p.usd))
      continue;
    if (p.px < loBound || p.px > hiBound) continue;
    const idx = Math.floor(p.px / bin);
    let cell = map.get(idx);
    if (!cell) {
      cell = { longUsd: 0, shortUsd: 0 };
      map.set(idx, cell);
    }
    if (p.longSide) cell.longUsd += p.usd;
    else cell.shortUsd += p.usd;
  }
  const idxs = [...map.keys()].sort((a, b) => a - b);
  const bands: HtBand[] = [];
  const mags: number[] = [];
  for (const idx of idxs) {
    const cell = map.get(idx)!;
    bands.push({
      lo: idx * bin,
      hi: (idx + 1) * bin,
      longUsd: cell.longUsd,
      shortUsd: cell.shortUsd,
    });
    const mag = Math.max(cell.longUsd, cell.shortUsd);
    if (mag > 0) mags.push(mag);
  }
  return { bands, fetchedAt: now, ref: percentile(mags, 0.95) };
}

function num(v: unknown): number {
  if (typeof v === "number" && Number.isFinite(v)) return v;
  if (typeof v === "boolean") return v ? 1 : 0;
  if (typeof v === "string") {
    const n = Number(v.replace(/,/g, "").trim());
    return Number.isFinite(n) ? n : 0;
  }
  return 0;
}

function str(v: unknown): string {
  if (typeof v === "string") return v;
  if (typeof v === "number" && Number.isFinite(v)) return String(v);
  if (typeof v === "boolean") return v ? "true" : "false";
  return "";
}

function truthy(v: unknown): boolean {
  if (v === true || v === 1) return true;
  const s = str(v).trim().toLowerCase();
  return s === "true" || s === "1" || s === "yes";
}

function field(r: Record<string, unknown>, ...names: string[]): unknown {
  for (const name of names) {
    if (name in r && r[name] != null && r[name] !== "") return r[name];
  }
  const keys = Object.keys(r);
  for (const name of names) {
    const want = name.toLowerCase().replace(/[_\s-]/g, "");
    for (const k of keys) {
      if (k.toLowerCase().replace(/[_\s-]/g, "") === want) {
        const v = r[k];
        if (v != null && v !== "") return v;
      }
    }
  }
  return undefined;
}

function splitCsvLine(line: string): string[] {
  const out: string[] = [];
  let cur = "";
  let q = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i]!;
    if (q) {
      if (c === '"') {
        if (line[i + 1] === '"') {
          cur += '"';
          i++;
        } else q = false;
      } else cur += c;
    } else if (c === '"') q = true;
    else if (c === ",") {
      out.push(cur);
      cur = "";
    } else cur += c;
  }
  out.push(cur);
  return out;
}

function parseCsvObjects(text: string): Record<string, string>[] {
  const lines = text.split(/\r?\n/).filter((l) => l.length > 0);
  if (lines.length < 2) return [];
  const headers = splitCsvLine(lines[0]!).map((h) => h.trim());
  const rows: Record<string, string>[] = [];
  for (let i = 1; i < lines.length; i++) {
    const cols = splitCsvLine(lines[i]!);
    const row: Record<string, string> = {};
    for (let c = 0; c < headers.length; c++) row[headers[c]!] = cols[c] ?? "";
    rows.push(row);
  }
  return rows;
}

function asObjects(payload: unknown): Record<string, unknown>[] {
  if (Array.isArray(payload)) return payload as Record<string, unknown>[];
  if (payload && typeof payload === "object") {
    const o = payload as Record<string, unknown>;
    for (const k of ["data", "metrics", "fills", "positions", "orders", "rows"]) {
      if (Array.isArray(o[k])) return o[k] as Record<string, unknown>[];
    }
  }
  return [];
}

function parseBody(text: string): Record<string, unknown>[] {
  const t = text.trim();
  if (!t) return [];
  if (t[0] === "[" || t[0] === "{") {
    try {
      const parsed = JSON.parse(t) as unknown;
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        const o = parsed as Record<string, unknown>;
        const fileUrl = o.downloadUrl || o.download_url || o.url;
        if (typeof fileUrl === "string" && /^https?:\/\//.test(fileUrl)) {
          const err = new Error("proxy did not unwrap downloadUrl");
          (err as Error & { code: number }).code = HT_ERROR;
          throw err;
        }
      }
      return asObjects(parsed);
    } catch (e) {
      if ((e as { code?: number }).code === HT_ERROR) throw e;
      return [];
    }
  }
  return parseCsvObjects(t) as Record<string, unknown>[];
}

function isLongSide(side: string): boolean {
  const s = side.toLowerCase();
  return s === "long" || s === "buy" || s === "b" || s === "bid";
}

function isShortSide(side: string): boolean {
  const s = side.toLowerCase();
  return s === "short" || s === "sell" || s === "a" || s === "ask";
}

function positionsToPoints(rows: Record<string, unknown>[], binArg?: number): HtLayer {
  const pts: { px: number; usd: number; longSide: boolean }[] = [];
  for (const r of rows) {
    const px = num(field(r, "liquidationPrice", "liquidationPx", "liqPx",
                        "liquidation_price", "liqPrice"));
    if (!(px > 0)) continue;
    const side = str(field(r, "side"));
    const usd = num(field(r, "value", "positionValue", "sizeUsd", "positionValueUsd"));
    const sz = num(field(r, "size", "szi", "sz"));
    const notional = usd > 0 ? usd : sz * px;
    if (!(notional > 0)) continue;
    const longSide = isShortSide(side) ? false : isLongSide(side) ? true : sz >= 0;
    pts.push({ px, usd: Math.abs(notional), longSide });
  }
  return binPoints(pts, binArg);
}

function isTakeProfit(r: Record<string, unknown>): boolean {
  const t = str(field(r, "orderType", "type", "order_type")).toLowerCase();
  return t.includes("take profit") || t.includes("takeprofit") || t === "tp";
}

function isStopOrder(r: Record<string, unknown>): boolean {
  if (isTakeProfit(r)) return false;
  const t = str(field(r, "orderType", "type", "order_type")).toLowerCase();
  if (t.includes("stop")) return true;
  if (truthy(field(r, "isTrigger", "is_trigger", "trigger"))) return true;
  if (truthy(field(r, "isPositionTpsl", "is_position_tpsl"))) return true;
  return num(field(r, "triggerPx", "trigger_px", "triggerPrice", "stopPx", "stopPrice")) > 0;
}

function ordersToPoints(rows: Record<string, unknown>[], coin: string): HtLayer {
  const want = coin.toUpperCase();
  const pts: { px: number; usd: number; longSide: boolean }[] = [];
  for (const r of rows) {
    const rowCoin = str(field(r, "coin", "symbol")).toUpperCase();
    if (rowCoin && rowCoin !== want && rowCoin !== `${want}USDT`) continue;
    if (!isStopOrder(r)) continue;
    let px = num(field(r, "triggerPx", "trigger_px", "triggerPrice", "stopPx", "stopPrice"));
    if (!(px > 0)) px = num(field(r, "limitPx", "limit_px", "price"));
    if (!(px > 0)) continue;
    const sz = Math.abs(num(field(r, "sz", "size", "origSz", "orig_sz", "qty")));
    if (!(sz > 0)) continue;
    const side = str(field(r, "side"));
    // Ask (A) = sell trigger ≈ long stop. Bid (B) = buy trigger ≈ short stop.
    const longSide = isShortSide(side) ? true : isLongSide(side) ? false : true;
    pts.push({ px, usd: sz * px, longSide });
  }
  return binPoints(pts);
}

async function htFetch(path: string, token: string): Promise<string> {
  const headers: Record<string, string> = {
    Authorization: `Bearer ${token}`,
    Accept: "application/json, text/csv;q=0.9, */*;q=0.8",
  };
  const res = await fetch(`${PROXY}${path}`, { headers });
  if (res.status === 401 || res.status === 403) {
    const err = new Error("unauthorized");
    (err as Error & { code: number }).code = HT_ERROR;
    throw err;
  }
  if (res.status === 429) {
    const err = new Error("quota");
    (err as Error & { code: number }).code = HT_QUOTA_HIT;
    throw err;
  }
  if (!res.ok) {
    const err = new Error(`http ${res.status}`);
    (err as Error & { code: number }).code = HT_ERROR;
    throw err;
  }
  return res.text();
}

async function fetchLayer(kind: CacheKind, coin: string, token: string): Promise<HtLayer> {
  if (kind === "liq") {
    const text = await htFetch(`/api/external/positions/open/coin/${encodeURIComponent(coin)}`, token);
    return positionsToPoints(parseBody(text));
  }
  const text = await htFetch(
    `/api/external/orders/5m-snapshots/coins/${encodeURIComponent(coin.toLowerCase())}/download`,
    token,
  );
  const rows = parseBody(text);
  const layer = ordersToPoints(rows, coin);
  (self as unknown as Worker).postMessage({
    kind: "diag",
    text: `ht sl ${coin}: ${rows.length} rows → ${layer.bands.length} bands`,
  });
  return layer;
}

function pack(layer: HtLayer | null): Float64Array {
  if (!layer || layer.bands.length === 0) return new Float64Array(0);
  const out = new Float64Array(layer.bands.length * 4);
  for (let i = 0; i < layer.bands.length; i++) {
    const b = layer.bands[i]!;
    const o = i * 4;
    out[o] = b.lo;
    out[o + 1] = b.hi;
    out[o + 2] = b.longUsd;
    out[o + 3] = b.shortUsd;
  }
  return out;
}

function postState(state: HtState): void {
  const sig = JSON.stringify({
    coin: state.coin,
    status: state.status,
    used: state.used,
    liqN: state.liq?.bands.length ?? 0,
    slN: state.sl?.bands.length ?? 0,
    liqAt: state.liq?.fetchedAt ?? 0,
    slAt: state.sl?.fetchedAt ?? 0,
    refL: state.liq?.ref ?? 0,
    refS: state.sl?.ref ?? 0,
  });
  if (sig === lastPosted) return;
  lastPosted = sig;
  const liq = pack(state.liq);
  const sl = pack(state.sl);
  const msg = {
    kind: "htMap",
    coin: state.coin,
    status: state.status,
    used: state.used,
    quota: state.quota,
    liqAt: state.liq?.fetchedAt ?? 0,
    slAt: state.sl?.fetchedAt ?? 0,
    liqRef: state.liq?.ref ?? 0,
    slRef: state.sl?.ref ?? 0,
    liq,
    sl,
    sym: state.sym,
  };
  const xfer: Transferable[] = [];
  if (liq.byteLength) xfer.push(liq.buffer);
  if (sl.byteLength) xfer.push(sl.buffer);
  (self as unknown as Worker).postMessage(msg, xfer);
}

async function cachedLayer(kind: CacheKind, coin: string): Promise<HtLayer | null> {
  const rec = await idbGet<LayerRec>(layerKey(kind, coin));
  if (!rec || rec.coin !== coin) return null;
  return { bands: rec.bands, fetchedAt: rec.fetchedAt, ref: rec.ref };
}

function fresh(layer: HtLayer | null, now: number): boolean {
  return !!layer && now - layer.fetchedAt < HT_LIVE_MS;
}

function remainingTtl(layer: HtLayer | null, now: number): number {
  if (!layer) return HT_LIVE_MS;
  return Math.max(30_000, HT_LIVE_MS - (now - layer.fetchedAt));
}

async function ensureLayer(
  kind: CacheKind,
  coin: string,
  token: string,
  quota: QuotaRec,
): Promise<{ layer: HtLayer | null; quota: QuotaRec; status: number }> {
  const now = Date.now();
  const cached = await cachedLayer(kind, coin);
  if (fresh(cached, now)) return { layer: cached, quota, status: HT_OK };

  const remaining = HT_QUOTA - quota.used;
  if (remaining <= 0) return { layer: cached, quota, status: HT_QUOTA_HIT };
  // Soft cap: keep serving cache; only spend a request if we have nothing.
  if (quota.used >= HT_SOFT_CAP && cached) return { layer: cached, quota, status: HT_OK };

  try {
    const layer = await fetchLayer(kind, coin, token);
    const next = await bumpQuota();
    await idbSet(layerKey(kind, coin), {
      coin,
      kind,
      fetchedAt: layer.fetchedAt,
      ref: layer.ref,
      bands: layer.bands,
    } satisfies LayerRec);
    return { layer, quota: next, status: HT_OK };
  } catch (e) {
    const code = (e as { code?: number }).code;
    const msg = e instanceof Error ? e.message : String(e);
    (self as unknown as Worker).postMessage({
      kind: "diag",
      text: `ht ${kind} ${coin}: ${msg}`,
    });
    if (code === HT_NO_TOKEN) return { layer: cached, quota, status: HT_NO_TOKEN };
    if (code === HT_QUOTA_HIT) {
      const next = { ...quota, used: HT_QUOTA };
      await idbSet("quota", next);
      return { layer: cached, quota: next, status: HT_QUOTA_HIT };
    }
    return { layer: cached, quota, status: cached ? HT_OK : HT_ERROR };
  }
}

function nextWake(liq: HtLayer | null, sl: HtLayer | null, wantLiq: boolean, wantSl: boolean): number {
  const now = Date.now();
  let wait = HT_LIVE_MS;
  if (wantLiq) wait = Math.min(wait, remainingTtl(liq, now));
  if (wantSl) wait = Math.min(wait, remainingTtl(sl, now));
  return wait;
}

async function tick(version: number): Promise<void> {
  try {
    await tickInner(version);
  } catch {
    if (version !== configVersion) return;
    const coin = coinOf(cfg.sym);
    postState({
      coin, sym: cfg.sym, status: HT_ERROR, used: 0, quota: HT_QUOTA,
      liq: null, sl: null,
    });
  }
}

async function tickInner(version: number): Promise<void> {
  const { token, sym, liq: wantLiq, sl: wantSl } = cfg;
  const coin = coinOf(sym);
  if (!wantLiq && !wantSl) return;
  if (!token) {
    const quota = await loadQuota();
    if (version !== configVersion) return;
    postState({
      coin, sym, status: HT_NO_TOKEN, used: quota.used, quota: HT_QUOTA,
      liq: null, sl: null,
    });
    return;
  }

  let quota = await loadQuota();
  let status = HT_OK;
  let liq: HtLayer | null = null;
  let sl: HtLayer | null = null;

  if (wantLiq) {
    const r = await ensureLayer("liq", coin, token, quota);
    liq = r.layer;
    quota = r.quota;
    if (r.status !== HT_OK) status = r.status;
  }
  if (wantSl && status !== HT_NO_TOKEN) {
    const r = await ensureLayer("sl", coin, token, quota);
    sl = r.layer;
    quota = r.quota;
    if (status === HT_OK && r.status !== HT_OK) status = r.status;
  }

  // A symbol/token/toggle change can arrive while IndexedDB or the network is
  // pending. Never publish or schedule from that obsolete request; run()'s
  // completion handler immediately services the newest configuration.
  if (version !== configVersion) return;
  postState({ coin, sym, status, used: quota.used, quota: HT_QUOTA, liq, sl });

  if (timer) clearTimeout(timer);
  timer = setTimeout(() => {
    timer = null;
    void run();
  }, nextWake(liq, sl, wantLiq, wantSl));
}

function run(): Promise<void> {
  if (inflight) return inflight;
  const version = configVersion;
  inflight = tick(version).finally(() => {
    inflight = null;
    if (version !== configVersion) void run();
  });
  return inflight;
}

export function configureHt(next: HtConfig): void {
  let token = next.token.trim().replace(/\s+/g, "");
  if (token.toLowerCase().startsWith("bearer")) token = token.slice(6);
  const same =
    token === cfg.token &&
    next.sym === cfg.sym &&
    next.liq === cfg.liq &&
    next.sl === cfg.sl;
  cfg = { ...next, token };
  if (same && timer) return;
  ++configVersion;
  if (timer) {
    clearTimeout(timer);
    timer = null;
  }
  lastPosted = "";
  void run();
}
