const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });
  const dims = () => p.evaluate(async () => {
    const st = await pc.getStats(); let v = null;
    st.forEach(x => { if (x.type === 'inbound-rtp' && x.kind === 'video') v = x; });
    return v ? `${v.frameWidth}x${v.frameHeight} (${v.framesDecoded} frames)` : 'none';
  });
  const set = (r) => p.evaluate(async (res) => {
    const t = await (await fetch('/login', {method:'POST', body:'changeme'})).text();
    const s = await fetch('/resolution', {method:'POST', body:res, headers:{'X-Player-Token':t}});
    return s.status;
  }, r);
  await p.waitForTimeout(3000);
  console.log('  depart :', await dims());
  for (const r of ['720', '480', '1080']) {
    console.log(`  -> ${r}p (HTTP ${await set(r)})`);
    await p.waitForTimeout(5000);
    console.log('     recu :', await dims(), ' actif:', (await (await p.goto('http://127.0.0.1:5080/resolution')).text()).trim());
    await p.goBack(); await p.waitForTimeout(3000);
  }
  await b.close();
})();
