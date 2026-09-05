// cerium dev server — zero-dep static file server with correct WASM MIME.
// usage: node tools/serve.mjs [port]   (default 8788)
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, sep } from 'node:path';
import { gunzipSync } from 'node:zlib';

const root = process.cwd();
const port = Number(process.argv[2] || 8788);
const HT_UPSTREAM = 'https://ht-api.coinmarketman.com';

const mime = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.json': 'application/json',
  '.css': 'text/css',
  '.txt': 'text/plain',
};

const isolation = {
  'cache-control': 'no-cache',
  // cross-origin isolation: enables SharedArrayBuffer (feeds worker → WASM ring)
  'cross-origin-opener-policy': 'same-origin',
  'cross-origin-embedder-policy': 'require-corp',
};

function isolationHeaders(extra = {}) {
  return { ...isolation, ...extra };
}

function maybeGunzip(buf) {
  if (buf.length >= 2 && buf[0] === 0x1f && buf[1] === 0x8b) {
    try { return gunzipSync(buf); } catch { /* keep compressed */ }
  }
  return buf;
}

function findDownloadUrl(body) {
  if (!body || typeof body !== 'object') return null;
  const top = body.downloadUrl || body.download_url || body.url;
  if (typeof top === 'string' && /^https?:\/\//.test(top)) return top;
  const snap = body.snapshot;
  if (snap && typeof snap === 'object') {
    const nested = snap.downloadUrl || snap.download_url || snap.url;
    if (typeof nested === 'string' && /^https?:\/\//.test(nested)) return nested;
  }
  return null;
}

function snapshotTimeMs(body) {
  const snap = body && body.snapshot;
  const raw = (snap && (snap.completedAt || snap.createdAt || snap.capturedAt ||
                        snap.timestamp || snap.ts || snap.stateVersion)) ||
              (body && (body.completedAt || body.timestamp || body.ts));
  if (typeof raw === 'number' && Number.isFinite(raw)) return raw > 1e12 ? raw : raw * 1000;
  if (typeof raw === 'string') {
    const n = Date.parse(raw);
    if (Number.isFinite(n)) return n;
  }
  return 0;
}

async function proxyHypertracker(req, res, url) {
  const path = url.pathname.slice(3) || '/'; // drop /ht
  const target = HT_UPSTREAM + path + url.search;
  const headers = {};
  if (req.headers.authorization) headers.authorization = req.headers.authorization;
  if (req.headers.accept) headers.accept = req.headers.accept;
  const keepMeta = req.headers['x-ht-keep-meta'] === '1';
  try {
    let upstream = await fetch(target, { method: 'GET', headers, redirect: 'follow' });
    let buf = maybeGunzip(Buffer.from(await upstream.arrayBuffer()));
    // HT download endpoints return `{ downloadUrl }` — follow it even when
    // Content-Type is octet-stream. Prefer the file over any inline preview
    // `metrics` array so we don't bin two sample rows and call it a map.
    const head = buf.subarray(0, Math.min(buf.length, 8)).toString('utf8').trimStart();
    let meta = null;
    if (head.startsWith('{') || head.startsWith('[')) {
      try {
        const body = JSON.parse(buf.toString('utf8'));
        const fileUrl = findDownloadUrl(body);
        if (typeof fileUrl === 'string') {
          meta = {
            nextCursor: body.nextCursor ?? body.next_cursor ?? null,
            ts: snapshotTimeMs(body),
          };
          upstream = await fetch(fileUrl, { method: 'GET', redirect: 'follow' });
          buf = maybeGunzip(Buffer.from(await upstream.arrayBuffer()));
        }
      } catch {
        // keep the original body
      }
    }
    if (keepMeta && meta) {
      const out = JSON.stringify({
        nextCursor: meta.nextCursor,
        ts: meta.ts,
        body: buf.toString('utf8'),
      });
      res.writeHead(200, isolationHeaders({
        'content-type': 'application/json; charset=utf-8',
        'cache-control': 'no-store',
      }));
      return res.end(out);
    }
    const outType = upstream.headers.get('content-type') || 'application/octet-stream';
    res.writeHead(upstream.status, isolationHeaders({
      'content-type': outType,
      'cache-control': 'no-store',
    }));
    res.end(buf);
  } catch (err) {
    res.writeHead(502, isolationHeaders({ 'content-type': 'text/plain; charset=utf-8' }));
    res.end(`hypertracker proxy: ${err && err.message ? err.message : 'fetch failed'}`);
  }
}

createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://localhost');
    let p = decodeURIComponent(url.pathname);
    if (p === '/ht' || p.startsWith('/ht/')) {
      if (req.method !== 'GET' && req.method !== 'HEAD') {
        res.writeHead(405, isolationHeaders());
        return res.end('method not allowed');
      }
      return proxyHypertracker(req, res, url);
    }
    if (p === '/') p = '/index.html';
    const file = normalize(join(root, p));
    if (file !== root && !file.startsWith(root + sep)) {
      res.writeHead(403);
      return res.end('forbidden');
    }
    const data = await readFile(file);
    res.writeHead(200, isolationHeaders({
      'content-type': mime[extname(file).toLowerCase()] || 'application/octet-stream',
    }));
    res.end(data);
  } catch {
    res.writeHead(404);
    res.end('not found');
  }
}).listen(port, () => console.log(`cerium dev server → http://localhost:${port}`));
