#!/usr/bin/env node
// Venue-contract guard: feeds/registry.ts VENUES order must exactly equal
// kVenues in src/data/feeds.cpp. Index i in one file is wire-protocol venue i
// in the other — any drift silently mismatches books/tapes across the SAB.
//
// Dependency-free (plain node, regex parsing). Checks:
//   - entry counts on both sides (and against EXPECTED_COUNT)
//   - registry `index:` fields are 0..N-1 in declaration order
//   - id / label / short agreement, position by position
//   - class flags (cls in C++: 1=spot 2=perp 4=dex) against the id-derived
//     expectation (registry.ts has no cls field; the mapping below is the
//     contract). Bump DEX_IDS / PERP_EXTRA if a new venue class appears.
//   - ETH-only venues actually guard their make() on sym === "ETH"
//   - src/app/symbols.h kNames / kVenueCounts match SYMBOLS and the
//     per-symbol venue support columns in registry.ts
//
// Exits 1 with a diff on any mismatch. Wired into build.sh before em++.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");

// Class-flag contract derived from venue id (must match kVenues cls values).
const DEX_IDS = new Set(["hyperliquid", "aster", "lighter", "dydx", "extended"]);
// Perps whose id does not end in "-perp".
const PERP_EXTRA = new Set(["deribit"]);
// Venues that only support ETH in registry.ts make() (see AGENTS.md).
const ETH_ONLY_IDS = new Set(["gate-perp", "mexc-perp", "coinbase-us-perp"]);

// Both sides must stay in lockstep; bump this when adding/removing a venue
// from BOTH files (that is exactly the situation this guard exists to catch).
const EXPECTED_COUNT = 27;

function expectedCls(id) {
  if (DEX_IDS.has(id)) return 4;
  if (id.endsWith("-perp") || PERP_EXTRA.has(id)) return 2;
  return 1;
}

const clsName = { 1: "spot(1)", 2: "perp(2)", 4: "dex(4)" };

function parseRegistry() {
  const src = readFileSync(join(ROOT, "feeds/registry.ts"), "utf8");
  const start = src.indexOf("export const VENUES");
  if (start < 0) throw new Error("feeds/registry.ts: `export const VENUES` not found");
  const end = src.indexOf("\n];", start);
  if (end < 0) throw new Error("feeds/registry.ts: VENUES array terminator `];` not found");
  const body = src.slice(start, end);
  // Every entry declares its header fields in this exact order; matching the
  // quad avoids brace-matching make() bodies full of nested config objects
  // and `${...}` interpolations.
  const headerRe = /index:\s*(\d+),\s*\n\s*id:\s*"([^"]+)",\s*\n\s*label:\s*"([^"]+)",\s*\n\s*short:\s*"([^"]+)",/g;
  const venues = [];
  for (const m of body.matchAll(headerRe))
    venues.push({ index: +m[1], id: m[2], label: m[3], short: m[4], offset: m.index });
  // Sanity: one `make:` per header quad found — if the file's shape changes,
  // fail loudly instead of silently checking a partial list.
  const makeCount = (body.match(/\n\s*make:\s*\(/g) || []).length;
  if (venues.length !== makeCount)
    throw new Error(
      `feeds/registry.ts: parsed ${venues.length} venue headers but found ${makeCount} make() functions — parser out of date`
    );
  return { venues, body };
}

function parseCpp() {
  const src = readFileSync(join(ROOT, "src/data/feeds.cpp"), "utf8");
  const start = src.indexOf("} kVenues[] = {");
  if (start < 0) throw new Error("src/data/feeds.cpp: `kVenues[] = {` not found");
  const end = src.indexOf("\n};", start);
  if (end < 0) throw new Error("src/data/feeds.cpp: kVenues terminator `};` not found");
  const body = src.slice(start, end);
  const entryRe = /\{[^{}]*\}/g;
  const venues = [];
  for (const m of body.matchAll(entryRe)) {
    const e = m[0].trim();
    if (e === "{") continue;
    const g = /^\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*\}$/.exec(e);
    if (!g) throw new Error(`src/data/feeds.cpp: unparseable kVenues entry: ${e}`);
    venues.push({ id: g[1], label: g[2], short: g[3], cls: +g[4] });
  }
  return venues;
}

const errors = [];
const { venues: ts, body: tsBody } = parseRegistry();
const cpp = parseCpp();

if (ts.length !== EXPECTED_COUNT)
  errors.push(`feeds/registry.ts has ${ts.length} VENUES, expected ${EXPECTED_COUNT} (if you added/removed a venue, update BOTH files and EXPECTED_COUNT here)`);
if (cpp.length !== EXPECTED_COUNT)
  errors.push(`src/data/feeds.cpp has ${cpp.length} kVenues, expected ${EXPECTED_COUNT} (if you added/removed a venue, update BOTH files and EXPECTED_COUNT here)`);
if (ts.length !== cpp.length)
  errors.push(`venue count mismatch: registry.ts=${ts.length} vs feeds.cpp=${cpp.length}`);

const n = Math.max(ts.length, cpp.length);
for (let i = 0; i < n; i++) {
  const t = ts[i];
  const c = cpp[i];
  const tsDesc = t ? `"${t.id}"` : "<missing>";
  const cppDesc = c ? `"${c.id}"` : "<missing>";
  if (t && t.index !== i)
    errors.push(`registry.ts venue #${i} ("${t.id}") declares index: ${t.index} — indices must equal declaration order 0..N-1`);
  if (t && c) {
    if (t.id !== c.id)
      errors.push(`order mismatch at index ${i}: registry.ts=${tsDesc} vs feeds.cpp=${cppDesc}`);
    if (t.label !== c.label)
      errors.push(`label mismatch at index ${i} (${t.id}): registry.ts="${t.label}" vs feeds.cpp="${c.label}"`);
    if (t.short !== c.short)
      errors.push(`short label mismatch at index ${i} (${t.id}): registry.ts="${t.short}" vs feeds.cpp="${c.short}"`);
    const want = expectedCls(t.id);
    if (c.cls !== want)
      errors.push(
        `class mismatch at index ${i} (${t.id}): feeds.cpp cls=${c.cls} but id implies ${clsName[want]}` +
          ` — update kVenues cls or the id-derived rules in tools/check-venues.mjs`
      );
  } else if (!t || !c) {
    errors.push(`venue missing on one side at index ${i}: registry.ts=${tsDesc} vs feeds.cpp=${cppDesc}`);
  }
}

for (const t of ts) {
  // Per-entry text runs from this header to the next header (or array end).
  const next = ts.find((o) => o.offset > t.offset);
  const text = tsBody.slice(t.offset, next ? next.offset : tsBody.length);
  const guarded = /sym\s*!==\s*"ETH"|sym\s*===\s*"ETH"/.test(text);
  if (ETH_ONLY_IDS.has(t.id) && !guarded)
    errors.push(`registry.ts venue "${t.id}" is ETH-only by contract but its make() has no \`sym === "ETH"\` guard`);
  if (!ETH_ONLY_IDS.has(t.id) && guarded)
    errors.push(`registry.ts venue "${t.id}" guards on ETH but is not in ETH_ONLY_IDS in tools/check-venues.mjs — update the contract list`);
}

function parseSymbolsHeader() {
  const src = readFileSync(join(ROOT, "src/app/symbols.h"), "utf8");
  const names =
    /kNames\[\]\s*=\s*\{([^}]*)\}/.exec(src)?.[1]?.match(/"([^"]+)"/g) ?? null;
  if (!names) throw new Error("src/app/symbols.h: kNames table not found");
  const countsRe = /kVenueCounts\[kCount\]\s*=\s*\{([^}]*)\}/.exec(src);
  if (!countsRe) throw new Error("src/app/symbols.h: kVenueCounts table not found");
  const counts = countsRe[1].split(",").map((v) => parseInt(v.trim(), 10));
  return { names: names.map((s) => s.slice(1, -1)), counts };
}

// Per-symbol venue support from registry.ts make() guards: an ETH-only venue
// supports symbol 0 only; every other venue trades all listed symbols.
function parseSymbolSupport(venues, body, symbols) {
  const counts = symbols.map(() => 0);
  for (const v of venues) {
    const next = venues.find((o) => o.offset > v.offset);
    const text = body.slice(v.offset, next ? next.offset : body.length);
    const ethOnly = /sym\s*===\s*"ETH"|sym\s*!==\s*"ETH"/.test(text);
    for (let s = 0; s < symbols.length; s++)
      if (!ethOnly || s === 0) counts[s]++;
  }
  return counts;
}

// symbols.h contract: the C++ side renders labels/counts from these literals.
{
  const { names: cppSymbols, counts: cppCounts } = parseSymbolsHeader();
  const full = readFileSync(join(ROOT, "feeds/registry.ts"), "utf8");
  const symStart = full.indexOf("export const SYMBOLS");
  if (symStart < 0) throw new Error("feeds/registry.ts: `export const SYMBOLS` not found");
  const symLine = full.slice(symStart, full.indexOf("\n", symStart));
  const tsSymbols = [...symLine.matchAll(/"([^"]+)"/g)].map((m) => m[1]);
  const support = parseSymbolSupport(ts, tsBody, tsSymbols);
  if (cppSymbols.length !== tsSymbols.length)
    errors.push(`symbols.h kNames has ${cppSymbols.length} entries vs registry.ts SYMBOLS ${tsSymbols.length}`);
  for (let i = 0; i < Math.min(cppSymbols.length, tsSymbols.length); i++)
    if (cppSymbols[i] !== tsSymbols[i])
      errors.push(`symbols.h kNames[${i}]="${cppSymbols[i]}" vs registry.ts SYMBOLS[${i}]="${tsSymbols[i]}"`);
  for (let i = 0; i < Math.min(cppCounts.length, support.length); i++)
    if (cppCounts[i] !== support[i])
      errors.push(`symbols.h kVenueCounts[${i}] (${cppSymbols[i] ?? "?"})=${cppCounts[i]} but registry.ts implies ${support[i]} — update kVenueCounts in src/app/symbols.h`);
}

if (errors.length) {
  console.error(`venue contract check FAILED (${errors.length} issue${errors.length === 1 ? "" : "s"}):`);
  for (const e of errors) console.error(`  - ${e}`);
  console.error("feeds/registry.ts VENUES and src/data/feeds.cpp kVenues must match exactly (see AGENTS.md venue index contract).");
  process.exit(1);
}

console.log(`venue contract ok: ${ts.length} venues, ids/order/class flags match`);
