// Binance USD-M klines (candle history) for the chart panel.
// Posted to the main thread as a packed Float64Array transfer:
// [openTimeMs, open, high, low, close, volume, takerBuyVolume] × n.

export async function fetchKlines(
  canon: string,
  intervalMin: number,
): Promise<Float64Array | null> {
  const sym = `${canon}USDT`;
  const url = `https://fapi.binance.com/fapi/v1/klines?symbol=${sym}&interval=${intervalMin}m&limit=1000`;
  try {
    const res = await fetch(url);
    if (!res.ok) return null;
    const raw: unknown = await res.json();
    if (!Array.isArray(raw)) return null;
    const out = new Float64Array(raw.length * 7);
    for (let i = 0; i < raw.length; i++) {
      const k = raw[i];
      if (!Array.isArray(k)) return null;
      out[i * 7 + 0] = Number(k[0]);
      out[i * 7 + 1] = Number(k[1]);
      out[i * 7 + 2] = Number(k[2]);
      out[i * 7 + 3] = Number(k[3]);
      out[i * 7 + 4] = Number(k[4]);
      out[i * 7 + 5] = Number(k[5]);
      out[i * 7 + 6] = Number(k[9]); // taker buy base volume → per-candle delta
    }
    return out;
  } catch {
    return null;
  }
}
