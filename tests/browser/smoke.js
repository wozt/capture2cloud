#!/usr/bin/env node
/* Browser smoke test: drives a real Chromium against the running
 * capture2cloud server and checks the things unit tests structurally
 * cannot -- that the page loads, that WebRTC actually negotiates, and
 * that video frames really arrive.
 *
 * This is the gap that bit us before: the STUN/ICE change left the
 * stream stuck on "connecting" while every unit test still passed,
 * because each piece was individually correct and only the assembled
 * whole was broken. That class of bug is exactly what this catches.
 *
 * Requires: capture2cloud running (with its capture device), and
 * `npm install` + `npx playwright install chromium` done once.
 *
 * Usage:
 *   node tests/browser/smoke.js [--headed] [--url http://localhost:5080]
 *
 * Exits non-zero if anything fails.
 */
const path = require('path');
const fs = require('fs');
const { chromium } = require('playwright');

const args = process.argv.slice(2);
const HEADED = args.includes('--headed');
const urlIdx = args.indexOf('--url');
const BASE_URL = urlIdx !== -1 ? args[urlIdx + 1] : 'http://localhost:5080';
const SHOT_DIR = path.join(__dirname, 'screenshots');

let passed = 0;
let failed = 0;
const failures = [];

function check(name, condition, detail) {
  if (condition) {
    passed++;
    console.log(`  ok   ${name}`);
  } else {
    failed++;
    const line = detail ? `${name} (${detail})` : name;
    failures.push(line);
    console.log(`  FAIL ${line}`);
  }
}

function group(name) {
  console.log(`\n${name}`);
}

async function shot(page, name) {
  fs.mkdirSync(SHOT_DIR, { recursive: true });
  const file = path.join(SHOT_DIR, `${name}.png`);
  await page.screenshot({ path: file });
  return file;
}

(async () => {
  const browser = await chromium.launch({
    headless: !HEADED,
    args: [
      /* The page gates playback behind a click because browsers block
       * autoplay with sound; this lets the test exercise the normal path
       * without simulating that gesture. */
      '--autoplay-policy=no-user-gesture-required',
      /* Headless Chromium has no real audio device; without this, audio
       * decoding can fail in ways that have nothing to do with our code. */
      '--use-fake-device-for-media-stream',
      '--use-fake-ui-for-media-stream'
    ]
  });

  const context = await browser.newContext();
  const page = await context.newPage();

  /* Surface page-side errors: a JS exception in app.js would otherwise
   * show up only as mysteriously missing UI. */
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  const consoleErrors = [];
  page.on('console', (m) => {
    if (m.type() === 'error') consoleErrors.push(m.text());
  });

  try {
    group('page loads');
    const resp = await page.goto(BASE_URL, { waitUntil: 'domcontentloaded', timeout: 15000 });
    check('HTTP 200', resp && resp.status() === 200, resp ? `status ${resp.status()}` : 'no response');
    check('title is Capture2Cloud', (await page.title()) === 'Capture2Cloud');

    /* app.js is a separate request; if it 404s the page looks fine but
     * does nothing, so assert the script actually ran. */
    await page.waitForFunction(() => typeof window.retry === 'function', null, { timeout: 5000 }).catch(() => {});
    const appJsRan = await page.evaluate(() => typeof window.start === 'function' || typeof window.pc !== 'undefined');
    check('app.js loaded and executed', appJsRan);

    group('WebRTC negotiation');
    /* The real assertion: the peer connection must reach connected, not
     * sit on "connecting" forever (the STUN regression's signature). */
    let connState = 'unknown';
    try {
      await page.waitForFunction(
        () => window.pc && (window.pc.connectionState === 'connected' || window.pc.connectionState === 'failed'),
        null,
        { timeout: 20000 }
      );
      connState = await page.evaluate(() => window.pc.connectionState);
    } catch (e) {
      connState = await page.evaluate(() => (window.pc ? window.pc.connectionState : 'no pc')).catch(() => 'unknown');
    }
    check('peer connection reaches "connected"', connState === 'connected', `state: ${connState}`);

    const iceState = await page.evaluate(() => (window.pc ? window.pc.iceConnectionState : 'no pc'));
    check('ICE connected', iceState === 'connected' || iceState === 'completed', `ice: ${iceState}`);

    group('media actually flows');
    /* Dimensions prove a decoded frame arrived -- an established
     * connection carrying nothing would still leave these at 0. */
    let dims = { w: 0, h: 0 };
    try {
      await page.waitForFunction(
        () => {
          const v = document.getElementById('v');
          return v && v.videoWidth > 0;
        },
        null,
        { timeout: 15000 }
      );
    } catch (e) {}
    dims = await page.evaluate(() => {
      const v = document.getElementById('v');
      return { w: v ? v.videoWidth : 0, h: v ? v.videoHeight : 0 };
    });
    check('video has real dimensions', dims.w > 0 && dims.h > 0, `${dims.w}x${dims.h}`);

    /* And that it keeps arriving: a single frame then a freeze would
     * pass the check above but is still broken. */
    const t1 = await page.evaluate(() => document.getElementById('v').currentTime);
    await page.waitForTimeout(2000);
    const t2 = await page.evaluate(() => document.getElementById('v').currentTime);
    check('playback advances (not frozen)', t2 > t1, `currentTime ${t1.toFixed(2)} -> ${t2.toFixed(2)}`);

    /* Bytes on the wire, straight from the browser's own stats. */
    const stats = await page.evaluate(async () => {
      const out = { videoBytes: 0, audioBytes: 0, frames: 0 };
      const s = await window.pc.getStats(null);
      s.forEach((r) => {
        if (r.type === 'inbound-rtp' && r.kind === 'video') {
          out.videoBytes = r.bytesReceived || 0;
          out.frames = r.framesDecoded || 0;
        }
        if (r.type === 'inbound-rtp' && r.kind === 'audio') out.audioBytes = r.bytesReceived || 0;
      });
      return out;
    });
    check('video bytes received', stats.videoBytes > 0, `${stats.videoBytes} bytes`);
    check('video frames decoded', stats.frames > 0, `${stats.frames} frames`);
    check('audio bytes received', stats.audioBytes > 0, `${stats.audioBytes} bytes`);

    group('gamepad DataChannel');
    const dcState = await page.evaluate(() =>
      window.gamepadChannel ? window.gamepadChannel.readyState : 'no channel'
    );
    check('gamepad DataChannel is open', dcState === 'open', `state: ${dcState}`);

    group('no page errors');
    check('no uncaught JS exceptions', pageErrors.length === 0, pageErrors.join(' | '));
    /* The on-page status line doubles as an error display (window.onerror
     * writes into it), so a JS error would show up there too. */
    const status = await page.evaluate(() => {
      const el = document.getElementById('st');
      return el ? el.textContent : '';
    });
    check('status line shows no JS error', !status.startsWith('JS error'), status);

    const file = await shot(page, 'smoke');
    console.log(`\nscreenshot: ${file}`);
  } catch (e) {
    failed++;
    failures.push(`unexpected exception: ${e.message}`);
    console.log(`\nFAILED WITH EXCEPTION: ${e.stack}`);
    try {
      await shot(page, 'smoke-failure');
    } catch (_) {}
  } finally {
    await browser.close();
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  if (consoleErrors.length) {
    console.log('\nbrowser console errors:');
    consoleErrors.forEach((e) => console.log(`  - ${e}`));
  }
  if (failed > 0) {
    console.log('\nFailures:');
    failures.forEach((f) => console.log(`  - ${f}`));
    process.exit(1);
  }
})();
