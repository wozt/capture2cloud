/* Measures the gap between consecutive PRESENTED frames, using
 * requestVideoFrameCallback -- the only way to see a hitch that lasts a
 * few frames. Sampling getStats() once a second cannot: 3 missed frames
 * out of 60 disappear into the average.
 *
 * Prints every gap above the threshold, with the elapsed time, so a
 * periodic hitch shows its period directly. */
const { chromium } = require('playwright');
const seconds = Number(process.argv[2] || 180);
const thresholdMs = Number(process.argv[3] || 50);
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  p.on('console', m => { const t = m.text(); if (t.startsWith('HITCH') || t.startsWith('SUM')) console.log(t); });
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });
  await p.evaluate((th) => {
    const v = document.getElementById('v');
    let prev = null, t0 = performance.now(), n = 0, worst = 0;
    const cb = (now, meta) => {
      n++;
      if (prev !== null) {
        const gap = now - prev;
        if (gap > worst) worst = gap;
        if (gap > th) {
          console.log(`HITCH t+${((now - t0) / 1000).toFixed(1)}s gap=${gap.toFixed(1)}ms ` +
            `presented=${meta.presentedFrames} dropped=${v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality().droppedVideoFrames : '?'}`);
        }
      }
      prev = now;
      v.requestVideoFrameCallback(cb);
    };
    v.requestVideoFrameCallback(cb);
    window.__sum = () => console.log(`SUM frames=${n} worst=${worst.toFixed(1)}ms elapsed=${((performance.now()-t0)/1000).toFixed(0)}s`);
  }, thresholdMs);
  await p.waitForTimeout(seconds * 1000);
  await p.evaluate(() => window.__sum());
  await p.waitForTimeout(500);
  console.log('--- fin ---');
  await b.close();
})();
