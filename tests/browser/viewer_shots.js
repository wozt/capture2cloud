/* Viewer vs player: what the settings bar actually offers. UI-level
 * access control is easy to get subtly wrong in a way only a screenshot
 * shows -- a missing CSS rule once left every gamepad control visible to
 * viewers even though the class was being applied correctly. */
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1400, height: 500 } });
  const errs = [];
  p.on('pageerror', e => errs.push(String(e)));
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForTimeout(3000);
  await p.mouse.move(700, 250);
  await p.evaluate(() => document.getElementById('bar').classList.add('visible'));

  const visible = async () => p.evaluate(() =>
    Array.from(document.getElementById('controlsRow').children)
      .filter(el => el.offsetParent !== null)
      .map(el => (el.textContent || '').trim().replace(/\s+/g, ' ').slice(0, 34))
      .filter(Boolean));

  console.log('--- VIEWER ---');
  console.log((await visible()).map(s => '  ' + s).join('\n'));
  await p.screenshot({ path: 'tests/browser/screenshots/bar-viewer.png', clip: await p.locator('#bar').boundingBox() });

  await p.evaluate(() => setPlayerUi(true));
  await p.waitForTimeout(300);
  console.log('--- PLAYER ---');
  console.log((await visible()).map(s => '  ' + s).join('\n'));
  await p.screenshot({ path: 'tests/browser/screenshots/bar-player.png', clip: await p.locator('#bar').boundingBox() });

  console.log(errs.length ? 'PAGE ERRORS: ' + errs.join('\n') : '\nno page errors');
  await b.close();
})();
