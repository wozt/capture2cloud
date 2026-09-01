/* Browser-side latency budget for the video path, averaged over a run.
 * These are the parts that sit between "the packet arrived" and "the
 * frame is on screen" -- the only part a viewer can perceive as the
 * stream lagging behind their input. */
const { chromium } = require('playwright');
const label = process.argv[2] || '';
const seconds = Number(process.argv[3] || 45);
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });
  await p.waitForTimeout(8000); // let the jitter buffer settle
  const grab = () => p.evaluate(async () => {
    const st = await pc.getStats();
    let v = null, pl = null;
    st.forEach(x => {
      if (x.type === 'inbound-rtp' && x.kind === 'video') v = x;
      if (x.type === 'media-playout') pl = x;
    });
    return { jbd: v.jitterBufferDelay || 0, jbc: v.jitterBufferEmittedCount || 0,
             dec: v.totalDecodeTime || 0, f: v.framesDecoded || 0,
             proc: v.totalProcessingDelay || 0, asm: v.totalAssemblyTime || 0,
             asmc: v.framesAssembledFromMultiplePackets || 0,
             jitter: v.jitter || 0 };
  });
  const a = await grab();
  await p.waitForTimeout(seconds * 1000);
  const z = await grab();
  const d = (k) => z[k] - a[k];
  const per = (num, den) => den > 0 ? (num / den * 1000) : 0;
  console.log(`[${label}] sur ${seconds}s, ${d('f')} images`);
  console.log(`  jitter buffer   : ${per(d('jbd'), d('jbc')).toFixed(1)} ms`);
  console.log(`  decodage        : ${per(d('dec'), d('f')).toFixed(2)} ms/image`);
  console.log(`  assemblage      : ${per(d('asm'), d('asmc')).toFixed(2)} ms`);
  console.log(`  delai total     : ${per(d('proc'), d('f')).toFixed(1)} ms  <-- reception -> pret a afficher`);
  console.log(`  gigue reseau    : ${(z.jitter * 1000).toFixed(2)} ms`);
  await b.close();
})();
