/* Samples the inbound video every 5s and prints fps + frame/byte deltas,
 * so a stream that dies partway through says exactly when it stopped and
 * whether packets were still arriving when it did. */
const { chromium } = require('playwright');
const seconds = Number(process.argv[2] || 90);
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });
  let prev = null;
  const t0 = Date.now();
  while ((Date.now() - t0) / 1000 < seconds) {
    await p.waitForTimeout(5000);
    const s = await p.evaluate(async () => {
      const st = await pc.getStats();
      let v = null;
      st.forEach(x => { if (x.type === 'inbound-rtp' && x.kind === 'video') v = x; });
      return v && { f: v.framesDecoded, b: v.bytesReceived, pk: v.packetsReceived,
                    drop: v.framesDropped, t: v.currentTime || Date.now() / 1000 };
    });
    if (!s) { console.log('no video stats'); continue; }
    const el = ((Date.now() - t0) / 1000).toFixed(0).padStart(3);
    if (prev) {
      const df = s.f - prev.f, db = s.b - prev.b, dp = s.pk - prev.pk;
      console.log(`t+${el}s  fps=${(df / 5).toFixed(1).padStart(5)}  frames+=${String(df).padStart(4)}  packets+=${String(dp).padStart(5)}  kB+=${String(Math.round(db / 1024)).padStart(5)}`);
    }
    prev = s;
  }
  await b.close();
})();
