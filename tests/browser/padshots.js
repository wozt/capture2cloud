const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch({ args: ['--use-fake-ui-for-media-stream'] });
  const p = await b.newPage({ viewport: { width: 1100, height: 800 } });
  const errs = [];
  p.on('pageerror', e => errs.push(String(e)));
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForTimeout(1500);
  // open the settings menu, then the controller test popup
  // The settings bar only takes clicks once it's revealed (mouse move).
  await p.mouse.move(550, 400);
  await p.evaluate(() => document.getElementById('bar').classList.add('visible'));
  // The controller test is player-only, so a viewer has no button to click.
  await p.evaluate(() => setPlayerUi(true));
  await p.click('#padtest-btn');
  await p.waitForTimeout(400);
  for (const layout of ['xbox', 'switch', 'ps5']) {
    await p.selectOption('#padtest-layout', layout);
    await p.waitForTimeout(400);
    const box = await p.locator('#padtest-box').boundingBox();
    await p.screenshot({ path: `tests/browser/screenshots/padtest-${layout}.png`, clip: box });
    const n = await p.locator('#padtest-svg *').count();
    console.log(`${layout}: ${n} svg elements, popup ${box.width}x${Math.round(box.height)}`);
  }
  console.log(errs.length ? 'PAGE ERRORS: ' + errs.join('\n') : 'no page errors');
  await b.close();
})();
