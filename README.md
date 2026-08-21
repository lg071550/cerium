# cerium

Browser-based crypto orderflow terminal. C++20 compiled to WebAssembly with a
custom WebGPU renderer and a custom immediate-mode UI toolkit — no DOM UI, no
framework, no backend.

## Status

Data plane phase: live Binance USDT-M perp feeds (depth + trades) stream from a
Web Worker over a SharedArrayBuffer binary ring into WASM-side books; the
Orderbook and Tape panels render real market data. Render core, UI toolkit, and
docking workspace as before. Next: venue fan-out (the aggbook adapter set),
merged book, symbol switching.

## Requirements

- Windows: Git Bash (or any POSIX-y shell)
- Everything else is vendored into `tools/` (see below)

## Setup

The toolchain is local to this repo (~2 GB, gitignored). To reproduce:

```sh
# 1. standalone python (bootstrap for emsdk) → tools/python
#    https://github.com/astral-sh/python-build-standalone (cpython-*-windows-msvc-install_only)

# 2. emscripten sdk → tools/emsdk
git clone --depth 1 https://github.com/emscripten-core/emsdk.git tools/emsdk
tools/python/python.exe tools/emsdk/emsdk.py install latest
tools/python/python.exe tools/emsdk/emsdk.py activate latest

# 3. esbuild binary → tools/esbuild
#    https://registry.npmjs.org/@esbuild/win32-x64/-/win32-x64-<ver>.tgz
#    extract so that tools/esbuild/esbuild.exe exists
```

Note: `emsdk.bat` hardcodes its own python discovery and this machine has no
system python — always invoke `emsdk.py` via `tools/python/python.exe`.

## Build & run

```sh
bash build.sh          # bundles feeds worker + release wasm;  bash build.sh dev for debug
node tools/serve.mjs   # → http://localhost:8788 (sets COOP/COEP for SharedArrayBuffer)
```

Open in Chrome or Edge (WebGPU required). Drag tabs between panels / to panel
or workspace edges to split; drag splitters to resize; layout persists in
localStorage (`cerium.layout.v1`).

Headless verification screenshot (no deps, drives Chrome via CDP):

```sh
node tools/shoot.mjs http://localhost:8788 build/shot.png 8000
```

## Layout

```
feeds/                  feeds worker (TS, bundled by esbuild)
  main.ts               worker bootstrap + venue registry + command poller
  wire.ts               SPSC binary ring writer over SharedArrayBuffer
  venues/               exchange adapters (vendored from aggbook)
src/
  main.cpp              entry + frame loop
  platform/             canvas/DPI/localStorage/cursor shell, input hooks
  gpu/wgpu_context.*    WebGPU instance/surface/device (version-sensitive API here)
  render/               pipelines, instanced quad + text batches, glyph atlas, renderer
  ui/                   theme, draw list, immediate-mode context, widgets
  dock/                 split/leaf tree, tab drag/drop state machine, JSON layout
  data/                 wire events, L2 book, tape ring, venue registry, JS bridge
  app/terminal.*        top bar, panel registry, panels
third_party/stb/        stb_truetype
assets/fonts/           IBM Plex Mono (OFL, see OFL.txt)
```

All coordinates in UI code are logical (CSS) px; the renderer scales by
devicePixelRatio. Text is rasterized at physical resolution.
