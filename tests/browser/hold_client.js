/* Keeps one client connected for N seconds, so CPU can be sampled under
 * a realistic load instead of idling with nobody watching. */
const { chromium } = require('playwright');
const seconds = Number(process.argv[2] || 20);
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('http://127.0.0.1:5080/', { waitUntil: 'networkidle' });
  await p.waitForFunction(() => pc && pc.connectionState === 'connected', { timeout: 20000 });
  console.log('client connected');
  await p.waitForTimeout(seconds * 1000);
  await b.close();
})();
