# cerium

A browser-based crypto orderflow terminal. Built with C++20, WebAssembly and
[Lanthanum](https://github.com/lg071550/lanthanum), with WebGPU rendering and
TypeScript market-data workers.

- Candles, custom timeframes, tick and volume bars, and footprints.
- TPO profiles with split view, value area, POC, single prints and tails.
- DOM, order books, trade tape and aggregated liquidations.
- VWAP, CVD, open interest, calendar levels and other indicators.
- Chart drawings, dockable panels and saved layouts.
- 27 exchange adapters; BTC, ETH and SOL coverage varies by venue.

Exchange feeds connect directly from the browser. No Cerium account is needed.
Optional HyperTracker overlays require a provider token.

## Build and run

The build script currently targets Windows with Git Bash. Install Git, Node.js,
npm and Python, then:

```sh
git clone --recurse-submodules https://github.com/lg071550/cerium.git
cd cerium

git clone https://github.com/emscripten-core/emsdk.git tools/emsdk
python tools/emsdk/emsdk.py install 6.0.6
python tools/emsdk/emsdk.py activate 6.0.6

npm install --prefix tools/esbuild --no-save @esbuild/win32-x64@0.28.2
cp tools/esbuild/node_modules/@esbuild/win32-x64/esbuild.exe tools/esbuild/esbuild.exe

bash build.sh
node tools/serve.mjs 8788
```

Open [localhost:8788](http://localhost:8788) in Chrome or Edge with WebGPU enabled.
Use `bash build.sh dev` for a debug build. Existing clones can fetch the renderer
with `git submodule update --init --recursive`.

The server supplies the COOP/COEP headers required for SharedArrayBuffer.
Other hosts must provide equivalent cross-origin isolation.

## Data

Historical candles and footprint bootstrap use Binance USD-M data. Live
orderflow can combine selected venues. Availability depends on venue support,
connectivity and regional restrictions.

Charts retain 2,000 candles; footprints retain up to 320,000 aggregate prints.
Older candles can fall outside loaded tick history. Cold history loads may take
minutes. DOM queue tiles are inferred from L2 changes, not market-by-order data.

## Renderer

Lanthanum handles quads, lines and text. Its reproducible
[benchmarks](third_party/lanthanum/bench/README.md) compare the renderer with
Dear ImGui; they do not measure the complete trading terminal.

## License

[MIT](LICENSE). See [THIRD_PARTY.md](THIRD_PARTY.md) for dependency and font notices.
