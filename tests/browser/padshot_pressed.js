const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1100, height: 800 } });
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForTimeout(1500);
  await p.mouse.move(550, 400);
  await p.evaluate(() => document.getElementById('bar').classList.add('visible'));
  await p.evaluate(() => setPlayerUi(true));
  await p.click('#padtest-btn');
  await p.waitForTimeout(300);
  // Force a state: A + RB held, LT half, right stick pushed up-right.
  await p.evaluate(() => {
    // The 60 fps loop recomputes gamepadState every frame; silence its
    // producer so the injected state survives long enough to look at.
    sendGamepadState = function () {};
    gamepadState[19] = 100; gamepadState[3] = 100; gamepadState[7] = 60;
    gamepadState[9] = 80; gamepadState[10] = 70; gamepadState[13] = 100;
    updatePadTest();
  });
  await p.waitForTimeout(200);
  const lit = await p.evaluate(() => document.querySelectorAll('#padtest-svg .pt-btn.on').length);
  const readout = await p.textContent('#padtest-readout');
  const box = await p.locator('#padtest-box').boundingBox();
  await p.screenshot({ path: 'tests/browser/screenshots/padtest-pressed.png', clip: box });
  console.log('lit buttons:', lit);
  console.log('readout:', readout);
  await b.close();
})();
