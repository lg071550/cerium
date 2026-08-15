#!/usr/bin/env bash
# cerium build script — compiles src/*.cpp to build/cerium.js + cerium.wasm via Emscripten.
# usage: bash build.sh [dev|release]   (default release)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# --- toolchain env ---------------------------------------------------------
PY="$ROOT/tools/python/python.exe"
if [[ -f "$PY" ]]; then
  # git-bash emscripten wrappers use `#!/usr/bin/env python3`; ensure one exists
  [[ -f "$ROOT/tools/python/python3.exe" ]] || cp "$PY" "$ROOT/tools/python/python3.exe"
  export PATH="$ROOT/tools/python:$PATH"
  export EMSDK_PY="$(cygpath -w "$PY" 2>/dev/null || echo "$PY")"
  export EMSDK_PYTHON="$EMSDK_PY"
fi

if [[ -f "$ROOT/tools/emsdk/.emscripten" ]]; then
  export EM_CONFIG="$ROOT/tools/emsdk/.emscripten"
fi
if [[ -f "$ROOT/tools/emsdk/emsdk_env.sh" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT/tools/emsdk/emsdk_env.sh" >/dev/null 2>&1 || true
fi
if ! command -v em++ >/dev/null 2>&1; then
  # manual fallback if emsdk_env.sh didn't take
  export EM_CONFIG="$ROOT/tools/emsdk/.emscripten"
  export PATH="$ROOT/tools/emsdk:$ROOT/tools/emsdk/upstream/emscripten:$PATH"
  for d in "$ROOT"/tools/emsdk/node/*/bin; do export PATH="$d:$PATH"; done
fi
if ! command -v em++ >/dev/null 2>&1; then
  echo "error: em++ not found. Install emsdk into tools/emsdk first (see README)." >&2
  exit 1
fi

# --- build -----------------------------------------------------------------
MODE="${1:-release}"
if [[ "$MODE" == "dev" ]]; then
  OPT="-O1 -g -sASSERTIONS=2"
else
  OPT="-O3 -flto -sASSERTIONS=0"
fi

mkdir -p build

# venue index contract: feeds/registry.ts VENUES must exactly equal kVenues in
# src/data/feeds.cpp (ids, order, labels, shorts, class flags). Fails hard
# before any compilation on drift.
node tools/check-venues.mjs

# feeds worker bundle (TS → JS)
ESBUILD="$ROOT/tools/esbuild/esbuild.exe"
if [[ ! -x "$ESBUILD" ]]; then
  echo "error: tools/esbuild/esbuild.exe missing (see README setup)" >&2
  exit 1
fi
"$ESBUILD" feeds/main.ts --bundle --format=esm --target=es2020 \
  --outfile=build/feeds.worker.js --log-level=warning --minify

# app sources + the lanthanum renderer submodule (third_party/lanthanum/src)
SOURCES=$(find src third_party/lanthanum/src -name '*.cpp' | sort)

# shellcheck disable=SC2086
em++ -std=c++20 $OPT -fno-exceptions -fno-rtti \
  -Wall -Wextra -Wno-unused-parameter \
  -Ithird_party -Ithird_party/lanthanum/src \
  --use-port=emdawnwebgpu \
  -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_cerium_on_candles,_cerium_perf \
  --embed-file assets@/assets \
  $SOURCES \
  -o build/cerium.js

echo "ok → build/cerium.js + build/cerium.wasm  ($MODE)"
