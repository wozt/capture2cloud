/* One sample a second, printing only what changed. A brief hitch has
 * several possible causes that look identical on screen, and each leaves
 * a different trace: packet loss (packetsLost/nack), a keyframe burst
 * (keyFramesDecoded + a bytes spike), the decoder falling behind
 * (framesDropped), or the browser's own freeze detector (freezeCount). */
const { chromium } = require('playwright');
const seconds = Number(process.argv[2] || 180);
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });
  const grab = () => p.evaluate(async () => {
    const st = await pc.getStats();
    let v = null;
    st.forEach(x => { if (x.type === 'inbound-rtp' && x.kind === 'video') v = x; });
    if (!v) return null;
    return { f: v.framesDecoded || 0, recv: v.framesReceived || 0, drop: v.framesDropped || 0,
             lost: v.packetsLost || 0, nack: v.nackCount || 0, pli: v.pliCount || 0,
             key: v.keyFramesDecoded || 0, freeze: v.freezeCount || 0,
             fdur: v.totalFreezesDuration || 0, b: v.bytesReceived || 0,
             asm: v.totalAssemblyTime || 0 };
  });
  let prev = await grab();
  const t0 = Date.now();
  while ((Date.now() - t0) / 1000 < seconds) {
    await p.waitForTimeout(1000);
    const s = await grab();
    if (!s || !prev) { prev = s; continue; }
    const d = {};
    for (const k of Object.keys(s)) d[k] = s[k] - prev[k];
    const t = ((Date.now() - t0) / 1000).toFixed(0).padStart(3);
    // Only print a line when something other than steady decoding happened.
    const notable = d.f < 55 || d.drop || d.lost || d.pli || d.key || d.freeze || d.nack;
    if (notable) {
      console.log(`t+${t}s fps=${String(d.f).padStart(3)} recv=${String(d.recv).padStart(3)}` +
        ` drop=${d.drop} lost=${d.lost} nack=${d.nack} pli=${d.pli} key=${d.key}` +
        ` freeze=${d.freeze} (+${(d.fdur).toFixed(2)}s) kB=${Math.round(d.b / 1024)}`);
    }
    prev = s;
  }
  console.log('--- fin ---');
  await b.close();
})();
