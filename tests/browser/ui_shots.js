#!/usr/bin/env node
/* Renders the UI in a real browser and saves screenshots, so the layout
 * can actually be looked at instead of guessed at from CSS.
 *
 * Motivation: the virtual gamepad overlay was positioned and sized over
 * many blind iterations (button overlap, thumb reach, the settings bar
 * being covered). Rendering it at a real phone-landscape size makes
 * those judgments direct.
 *
 * This is a *visual aid*, not a pass/fail suite -- it asserts only that
 * the elements it wants to photograph are actually there.
 *
 * Usage:
 *   node tests/browser/ui_shots.js [--password changeme] [--url ...]
 */
const path = require('path');
const fs = require('fs');
const { chromium } = require('playwright');

const args = process.argv.slice(2);
const pwIdx = args.indexOf('--password');
const PASSWORD = pwIdx !== -1 ? args[pwIdx + 1] : 'changeme';
const urlIdx = args.indexOf('--url');
const BASE_URL = urlIdx !== -1 ? args[urlIdx + 1] : 'http://localhost:5080';
const SHOT_DIR = path.join(__dirname, 'screenshots');

/* A common phone-in-landscape viewport: this is the case the overlay is
 * actually designed for. */
const PHONE_LANDSCAPE = { width: 915, height: 412 };

let failed = 0;
function check(name, ok, detail) {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` (${detail})` : ''}`);
  if (!ok) failed++;
}

(async () => {
  fs.mkdirSync(SHOT_DIR, { recursive: true });
  const browser = await chromium.launch({
    headless: true,
    args: ['--autoplay-policy=no-user-gesture-required', '--use-fake-device-for-media-stream']
  });

  const context = await browser.newContext({
    viewport: PHONE_LANDSCAPE,
    hasTouch: true,
    isMobile: true
  });
  const page = await context.newPage();

  try {
    await page.goto(BASE_URL, { waitUntil: 'domcontentloaded' });
    await page
      .waitForFunction(() => window.pc && window.pc.connectionState === 'connected', null, { timeout: 20000 })
      .catch(() => {});

    /* Log in: gamepad UI is players-only, so a viewer session would show
     * none of what we want to photograph. The settings bar only accepts
     * clicks once revealed (it's pointer-events:none otherwise), so move
     * the pointer to the top edge first -- same as a user would. The
     * page uses prompt() for the password, answered via the dialog
     * handler. */
    page.once('dialog', (d) => d.accept(PASSWORD));
    const authRequired = await page.evaluate(() => {
      const row = document.getElementById('auth-row');
      return row && !row.classList.contains('hidden');
    });
    if (authRequired) {
      await page.mouse.move(PHONE_LANDSCAPE.width / 2, 5);
      await page.waitForTimeout(300);
      await page.click('#login-btn');
      await page.waitForFunction(() => window.playerToken != null, null, { timeout: 10000 }).catch(() => {});
    }
    const isPlayer = await page.evaluate(() => document.getElementById('auth-state').textContent === 'player');
    check('logged in as player', isPlayer || !authRequired);

    /* 1. The virtual touch overlay, the thing tuned blind for so long. */
    await page.evaluate(() => {
      const sel = document.getElementById('gamepad-select');
      sel.value = window.VIRTUAL_GAMEPAD_ID;
      sel.onchange();
    });
    await page.waitForTimeout(300);
    const overlayVisible = await page.evaluate(() => !document.getElementById('vgp').classList.contains('hidden'));
    check('virtual overlay visible', overlayVisible);
    await page.screenshot({ path: path.join(SHOT_DIR, 'overlay-phone-landscape.png') });

    /* Report where each control actually landed, so overlaps or
     * off-screen controls are obvious without eyeballing pixels. */
    const boxes = await page.evaluate(() => {
      const ids = [
        'vgp-lstick', 'vgp-rstick', 'vgp-dpad', 'vgp-face',
        'vgp-lb', 'vgp-lt', 'vgp-rb', 'vgp-rt',
        'vgp-select', 'vgp-start', 'vgp-guide', 'vgp-l3', 'vgp-r3'
      ];
      /* Compare against the VISUAL viewport (what the user actually
       * sees) rather than window.innerWidth/Height, which on mobile is
       * the layout viewport and can be larger -- that discrepancy is
       * what previously let cut-off controls pass this check. */
      const vv = window.visualViewport;
      const vw = vv ? vv.width : window.innerWidth;
      const vh = vv ? vv.height : window.innerHeight;
      return ids.map((id) => {
        const el = document.getElementById(id);
        const r = el.getBoundingClientRect();
        return {
          id,
          x: Math.round(r.left), y: Math.round(r.top),
          w: Math.round(r.width), h: Math.round(r.height),
          offscreen: r.left < 0 || r.top < 0 || r.right > vw || r.bottom > vh
        };
      });
    });
    const off = boxes.filter((b) => b.offscreen);
    check('no control is off-screen', off.length === 0, off.map((b) => b.id).join(', '));

    /* Pairwise overlap between separate controls: two buttons sharing
     * pixels means one of them is hard to hit. */
    const overlaps = [];
    for (let i = 0; i < boxes.length; i++) {
      for (let j = i + 1; j < boxes.length; j++) {
        const a = boxes[i], b = boxes[j];
        const inter =
          Math.max(0, Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x)) *
          Math.max(0, Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y));
        if (inter > 0) overlaps.push(`${a.id}/${b.id}`);
      }
    }
    check('no two controls overlap', overlaps.length === 0, overlaps.join(', '));

    console.log('\n  control positions (viewport %dx%d):', PHONE_LANDSCAPE.width, PHONE_LANDSCAPE.height);
    boxes.forEach((b) => console.log(`    ${b.id.padEnd(12)} x=${String(b.x).padStart(4)} y=${String(b.y).padStart(4)} ${b.w}x${b.h}`));

    /* 2. The settings bar, which the overlay must not cover. */
    await page.mouse.move(PHONE_LANDSCAPE.width / 2, 5);
    await page.waitForTimeout(400);
    await page.screenshot({ path: path.join(SHOT_DIR, 'overlay-with-menu.png') });

    /* 3. Desktop view with the menu open, to check the controls row. */
    const desktop = await browser.newContext({ viewport: { width: 1280, height: 720 } });
    const dpage = await desktop.newPage();
    await dpage.goto(BASE_URL, { waitUntil: 'domcontentloaded' });
    await dpage.waitForTimeout(1500);
    await dpage.mouse.move(640, 5);
    await dpage.waitForTimeout(400);
    await dpage.screenshot({ path: path.join(SHOT_DIR, 'menu-desktop-viewer.png') });
    await desktop.close();

    console.log(`\n  screenshots written to ${SHOT_DIR}`);
  } catch (e) {
    failed++;
    console.log(`\nEXCEPTION: ${e.stack}`);
  } finally {
    await browser.close();
  }
  process.exit(failed > 0 ? 1 : 0);
})();
