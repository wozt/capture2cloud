/* Switches the capture format while a client is watching, and checks the
 * stream survives it. The device is closed and reopened underneath a
 * live encoder, so "the endpoint returns 204" is not evidence that the
 * picture came back. */
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });

  const frames = () => p.evaluate(async () => {
    const st = await pc.getStats();
    let v = null;
    st.forEach(x => { if (x.type === 'inbound-rtp' && x.kind === 'video') v = x; });
    return v ? v.framesDecoded : 0;
  });
  const post = (fmt) => p.evaluate(async (f) => {
    const r = await fetch('/login', { method: 'POST', body: 'changeme' });
    const tok = r.ok ? await r.text() : '';
    const res = await fetch('/capture-format', {
      method: 'POST', body: f, headers: tok ? { 'X-Player-Token': tok } : {} });
    return res.status;
  }, fmt);
  const active = () => p.evaluate(async () => (await (await fetch('/capture-format')).text()).trim());

  for (const fmt of ['mjpeg', 'yuyv']) {
    const before = await frames();
    console.log(`\n-> bascule vers ${fmt} (HTTP ${await post(fmt)})`);
    await p.waitForTimeout(4000);
    console.log(`   format actif: ${await active()}`);
    const mid = await frames();
    await p.waitForTimeout(4000);
    const after = await frames();
    console.log(`   images decodees: +${mid - before} pendant, +${after - mid} apres`);
    console.log(`   ${after - mid > 100 ? 'ok   le flux a survecu' : 'FAIL le flux ne redemarre pas'}`);
  }
  await b.close();
})();
