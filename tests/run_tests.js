#!/usr/bin/env node
// Test suite for app.js -- runs the real front-end code under Node
// against a minimal DOM/localStorage/Gamepad API stub (dom_stub.js).
//
// Why this exists: `node --check` only catches syntax errors. Actually
// executing app.js catches ordering bugs (using a var before its
// declaration runs), undefined DOM refs, and logic errors. This suite
// found a real one the day it was written: saveSettings() persisted to
// localStorage but never updated the in-memory `settings` object, so
// rebind profiles silently reverted on the next read.
//
// Usage:  node tests/run_tests.js        (from the capture2cloud dir)
//         node tests/run_tests.js --verbose
//
// Exit code is non-zero if any test fails, so it can gate a deploy.

const fs = require('fs');
const path = require('path');
const { createSandbox } = require('./dom_stub.js');

/* The order page.html loads them in. Kept in step with it by the test
 * "the page loads every front-end file, in this order" below: a file
 * added to one and not the other is exactly the kind of thing that
 * works in the tests and breaks in the browser. */
const WEB_FILES = [
  'main.js',
  'settings.js',
  'ui/controls.js',
  'ui/overlay.js',
  'authentication.js',
  'gamepad.js',
  'keyboard.js',
  'ui/panels.js',
  'touchpad.js',
  'webrtc.js',
];
const APP_JS = WEB_FILES.map((f) => path.join(__dirname, '..', 'web', f));
const VERBOSE = process.argv.includes('--verbose');

let passed = 0;
let failed = 0;
const failures = [];

function check(name, actual, expected) {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) {
    passed++;
    if (VERBOSE) console.log(`  ok   ${name}`);
  } else {
    failed++;
    failures.push(`${name}\n       expected: ${e}\n       actual:   ${a}`);
    console.log(`  FAIL ${name}: expected ${e}, got ${a}`);
  }
}

function group(name, fn) {
  console.log(`\n${name}`);
  try {
    fn();
  } catch (e) {
    failed++;
    failures.push(`${name} threw: ${e.stack}`);
    console.log(`  FAIL ${name} threw: ${e.message}`);
  }
}

// Builds a fake browser Gamepad object with all buttons/axes at rest.
function fakeGamepad(id, buttonCount = 17, axisCount = 4) {
  return {
    id,
    mapping: 'standard',
    buttons: Array.from({ length: buttonCount }, () => ({ pressed: false, value: 0 })),
    axes: Array.from({ length: axisCount }, () => 0)
  };
}

group('loads and initialises', () => {
  const s = createSandbox(APP_JS);
  check('no gamepad selected initially', s.selectedGamepadId, null);
  check('gamepadState is 21 slots', s.gamepadState.length, 21);
  check('virtual gamepad id constant', s.VIRTUAL_GAMEPAD_ID, '__virtual__');
  check('keyboard gamepad id constant', s.KEYBOARD_GAMEPAD_ID, '__keyboard__');
});

group('keyboard mode: key handling', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();
  check('mode switched', s.selectedGamepadId, '__keyboard__');

  // Default profile: W = left stick up (LY-), D = left stick right (LX+)
  s.kbHeldKeys['KeyW'] = true;
  s.kbHeldKeys['KeyD'] = true;
  s.recomputeKbState();
  check('W held -> LY = -100', s.kbState[12], -100);
  check('D held -> LX = +100', s.kbState[11], 100);

  // Opposite keys held together should cancel out, not stack.
  s.kbHeldKeys['KeyS'] = true;
  s.recomputeKbState();
  check('W+S held -> LY cancels to 0', s.kbState[12], 0);

  s.kbHeldKeys['KeyW'] = false;
  s.recomputeKbState();
  check('W released, S held -> LY = +100', s.kbState[12], 100);

  // Face buttons map to their GAMEPAD_XB360_* slots.
  s.kbHeldKeys['Space'] = true; // default: A
  s.recomputeKbState();
  check('Space held -> A slot (19) = 100', s.kbState[19], 100);
});

group('keyboard mode: leaving the mode clears state', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();
  s.kbHeldKeys['KeyW'] = true;
  s.recomputeKbState();
  check('LY set before leaving', s.kbState[12], -100);

  s.gamepadSelect.value = '';
  s.gamepadSelect.onchange();
  check('kbState cleared on leaving', Array.from(s.kbState).every((v) => v === 0), true);
  check('gamepadState cleared on leaving', Array.from(s.gamepadState).every((v) => v === 0), true);
});

group('keyboard rebind persists', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();

  check('default A binding', s.getActiveKeyboardProfile().keys.a, 'Space');
  s.rebindListening = { kind: 'keyboard', field: 'a' };
  s.applyKeyboardBinding('a', 'KeyZ');
  // This is the regression guard for the saveSettings() bug: re-reading
  // the profile must show the NEW binding, not the stale in-memory one.
  check('A rebound to KeyZ', s.getActiveKeyboardProfile().keys.a, 'KeyZ');
  check('listening state cleared', s.rebindListening, null);

  // And the new key must actually drive the slot.
  s.kbHeldKeys['KeyZ'] = true;
  s.recomputeKbState();
  check('KeyZ now drives A slot', s.kbState[19], 100);
  s.kbHeldKeys['KeyZ'] = false;
  s.kbHeldKeys['Space'] = true;
  s.recomputeKbState();
  check('old Space binding no longer drives A', s.kbState[19], 0);
});

group('mouse bindings', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();

  // Shooter-style defaults: fire on left click, aim on right click.
  check('default RT is left click', s.getActiveKeyboardProfile().mouse.rt, 'mouse0');
  check('default LT is right click', s.getActiveKeyboardProfile().mouse.lt, 'mouse2');

  // A bound mouse button drives its slot exactly like a key would.
  s.kbHeldMouse['mouse0'] = true;
  s.recomputeKbState();
  check('left click drives RT slot (4)', s.kbState[4], 100);
  s.kbHeldMouse['mouse0'] = false;
  s.recomputeKbState();
  check('releasing clears it', s.kbState[4], 0);

  // Key and mouse are alternatives for the same action, not additive
  // past the limit: holding both must still saturate at 100.
  s.kbHeldKeys['Digit3'] = true; // default key for RT
  s.kbHeldMouse['mouse0'] = true;
  s.recomputeKbState();
  check('key + mouse together saturate at 100', s.kbState[4], 100);
  s.kbHeldKeys['Digit3'] = false;
  s.kbHeldMouse['mouse0'] = false;

  // Rebinding through the panel persists and takes effect.
  s.applyMouseBinding('a', 'mouse1');
  check('A rebound to middle click', s.getActiveKeyboardProfile().mouse.a, 'mouse1');
  s.kbHeldMouse['mouse1'] = true;
  s.recomputeKbState();
  check('middle click now drives A slot (19)', s.kbState[19], 100);

  // Clearing a binding removes it rather than storing an empty string.
  s.applyMouseBinding('a', '');
  check('cleared binding is removed', s.getActiveKeyboardProfile().mouse.a, undefined);
  s.kbHeldMouse['mouse1'] = true;
  s.recomputeKbState();
  check('unbound middle click does nothing', s.kbState[19], 0);
});

group('mouse input is confined to the stream', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();

  // Not pointer-locked: the cursor is a normal cursor, so clicks belong
  // to whatever they land on. Capturing them here is what made the
  // settings bar unclickable in this mode.
  s.document.pointerLockElement = null;
  check('not playing while unlocked', s.mouseIsPlaying(), false);

  // Pointer locked = actually playing.
  s.document.pointerLockElement = s.video;
  check('playing while locked', s.mouseIsPlaying(), true);

  // Events landing on the settings bar or the rebind panel are UI, even
  // while locked.
  check('bar is a UI target', s.isUiTarget({ target: s.bar }), true);
  check('rebind panel is a UI target', s.isUiTarget({ target: s.rebindEl }), true);
  check('the video is not', s.isUiTarget({ target: s.video }), false);

  // Leaving pointer lock must release everything, or a button held when
  // the cursor was freed would stay pressed on the console.
  s.kbHeldMouse['mouse0'] = true;
  s.recomputeKbState();
  check('held mouse button drives its slot', s.kbState[4], 100);
  s.document.pointerLockElement = null;
  s.document.dispatch('pointerlockchange', {});
  check('unlocking releases held mouse buttons', s.kbState[4], 0);
});

group('settings migration v1 -> v2 (keyboard profile shape)', () => {
  // A v1 profile was a flat field->keycode map; it must survive as
  // `keys` rather than being discarded, or everyone's bindings vanish.
  const s = createSandbox(APP_JS, {
    'capture2cloud_settings': JSON.stringify({
      settingsVersion: 1,
      keyboardProfiles: { Default: { a: 'KeyJ', b: 'KeyK' } },
      activeKeyboardProfile: 'Default'
    })
  });
  const p = s.getActiveKeyboardProfile();
  check('old key bindings preserved', p.keys.a, 'KeyJ');
  check('second binding preserved', p.keys.b, 'KeyK');
  check('mouse map created empty', JSON.stringify(p.mouse), '{}');
  check('stamped with the current version', s.settings.settingsVersion, s.SETTINGS_VERSION);
});

group('X/Y compensation is per controller, not global', () => {
  // Applying the Xbox pad's transposed X/Y to every controller is what
  // made square and triangle come out swapped on a PS5 DualSense: that
  // pad reports a proper standard mapping and needs no correction.
  const s = createSandbox(APP_JS);
  check('Xbox pad needs the swap', s.gamepadNeedsXYSwap('Xbox Wireless Controller'), true);
  check('detected by Microsoft vendor id too',
    s.gamepadNeedsXYSwap('045e-0b13-Controller'), true);
  check('XInput device needs it', s.gamepadNeedsXYSwap('Some XInput PAD'), true);
  check('DualSense does not', s.gamepadNeedsXYSwap(
    'DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)'), false);
  check('unknown pad is assumed standard', s.gamepadNeedsXYSwap('Generic Pad'), false);

  const xboxProfile = s.defaultGamepadProfile('Xbox Wireless Controller');
  const ps5Profile = s.defaultGamepadProfile('DualSense Wireless Controller');
  check('Xbox: Y slot reads the LEFT button (transposed)', xboxProfile.y.index, 2);
  check('Xbox: X slot reads the TOP button (transposed)', xboxProfile.x.index, 3);
  check('PS5: Y slot reads the top button', ps5Profile.y.index, 3);
  check('PS5: X slot reads the left button', ps5Profile.x.index, 2);

  // The end-to-end consequence: triangle (top) must arrive as triangle.
  const ps5 = fakeGamepad('DualSense Wireless Controller');
  const out = new Int8Array(21);
  ps5.buttons[3].pressed = true; // triangle
  s.buildStateFromGamepad(ps5, out);
  check('PS5 triangle -> Y slot, not X', out[17], 100);
  check('PS5 triangle leaves X alone', out[20], 0);

  const xbox = fakeGamepad('Xbox Wireless Controller');
  out.fill(0);
  xbox.buttons[3].pressed = true;
  s.buildStateFromGamepad(xbox, out);
  check('Xbox button 3 still compensated to X', out[20], 100);
});

group('rebinding cannot corrupt the defaults', () => {
  // getGamepadProfile() caches the defaults per controller id so the
  // 60 Hz read path stops rebuilding 22 objects a frame. That makes the
  // returned object shared, so a caller writing through it would rewrite
  // what "default" means for that pad.
  const s = createSandbox(APP_JS);
  const id = 'Generic Pad';
  const before = s.getGamepadProfile(id);
  check('default A comes from button 0', before.a.index, 0);

  s.gamepadSelect.value = id;
  s.getSelectedGamepad = () => ({ id: id, buttons: [], axes: [] });
  s.applyGamepadBinding('a', { type: 'button', index: 9 });

  check('the rebind was saved', s.settings.gamepadProfiles[id].a.index, 9);
  check('the shared defaults were NOT rewritten',
    s.defaultProfileCache[id].a.index, 0);
  check('a second pad still gets clean defaults',
    s.getGamepadProfile('Another Pad').a.index, 0);
});

group('gamepad: default profile mapping', () => {
  const s = createSandbox(APP_JS);
  const gp = fakeGamepad('TestPad');
  const out = new Int8Array(21);

  s.buildStateFromGamepad(gp, out);
  check('nothing pressed -> all zero', Array.from(out).every((v) => v === 0), true);

  gp.buttons[0].pressed = true;
  s.buildStateFromGamepad(gp, out);
  check('button 0 -> A slot (19)', out[19], 100);

  // Standard mapping: 0=bottom, 1=right, 2=left, 3=top. A pad that
  // reports itself properly is passed through untouched.
  gp.buttons[0].pressed = false;
  gp.buttons[2].pressed = true;
  s.buildStateFromGamepad(gp, out);
  check('button 2 (left) -> X slot (20)', out[20], 100);
  check('button 2 does NOT reach Y', out[17], 0);

  gp.buttons[2].pressed = false;
  gp.buttons[3].pressed = true;
  s.buildStateFromGamepad(gp, out);
  check('button 3 (top) -> Y slot (17)', out[17], 100);
  gp.buttons[3].pressed = false;

  gp.buttons[2].pressed = false;
  gp.axes[0] = 1.0;
  s.buildStateFromGamepad(gp, out);
  check('axis 0 -> LX slot (11)', out[11], 100);
});

group('gamepad rebind: button and axis capture', () => {
  const s = createSandbox(APP_JS);
  const gp = fakeGamepad('RebindPad');
  s.navigator.getGamepads = () => [gp];
  s.selectedGamepadId = gp.id;

  // Button capture: first poll snapshots, then a NEW press is captured.
  s.rebindListening = { kind: 'gamepad', field: 'a' };
  s.rebindGpSnapshot = null;
  s.pollGamepadRebind();
  gp.buttons[5].pressed = true;
  s.pollGamepadRebind();
  check('A rebound to button 5', s.getGamepadProfile(gp.id).a, { type: 'button', index: 5 });
  check('listening cleared after capture', s.rebindListening, null);

  // The rebind must actually take effect in buildStateFromGamepad.
  const out = new Int8Array(21);
  s.buildStateFromGamepad(gp, out);
  check('button 5 now drives A slot', out[19], 100);
  gp.buttons[5].pressed = false;
  gp.buttons[0].pressed = true;
  s.buildStateFromGamepad(gp, out);
  check('button 0 no longer drives A slot', out[19], 0);

  // Axis capture.
  gp.buttons[0].pressed = false;
  s.rebindListening = { kind: 'gamepad', field: 'lx' };
  s.rebindGpSnapshot = null;
  s.pollGamepadRebind();
  gp.axes[2] = 0.9;
  s.pollGamepadRebind();
  check('LX rebound to axis 2', s.getGamepadProfile(gp.id).lx, { type: 'axis', index: 2 });
});

group('gamepad profiles are per-controller', () => {
  const s = createSandbox(APP_JS);
  const padA = fakeGamepad('PadA');
  const padB = fakeGamepad('PadB');
  s.navigator.getGamepads = () => [padA, padB];

  s.selectedGamepadId = padA.id;
  s.rebindListening = { kind: 'gamepad', field: 'a' };
  s.rebindGpSnapshot = null;
  s.pollGamepadRebind();
  padA.buttons[7].pressed = true;
  s.pollGamepadRebind();

  check('PadA rebound', s.getGamepadProfile('PadA').a, { type: 'button', index: 7 });
  check('PadB keeps the default', s.getGamepadProfile('PadB').a, { type: 'button', index: 0 });
});

group('virtual touch overlay merges with a real gamepad', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.VIRTUAL_GAMEPAD_ID;
  s.gamepadSelect.onchange();

  // Touch alone.
  s.vgpTouchState[19] = 100; // A via touch
  s.combineVirtualGamepadState();
  check('touch alone drives A', s.gamepadState[19], 100);

  // Real gamepad alone (different button).
  s.vgpTouchState[19] = 0;
  const gp = fakeGamepad('MergePad');
  gp.buttons[1].pressed = true; // B by default
  s.navigator.getGamepads = () => [gp];
  s.combineVirtualGamepadState();
  check('real gamepad alone drives B', s.gamepadState[18], 100);

  // Both at once, same slot: saturates at 100 rather than overflowing.
  s.vgpTouchState[18] = 100;
  s.combineVirtualGamepadState();
  check('touch + gamepad on same slot saturates at 100', s.gamepadState[18], 100);
});

group('video filter settings', () => {
  const s = createSandbox(APP_JS);
  check(
    'default filter applied to video',
    s.video.style.filter,
    'brightness(100%) contrast(100%) saturate(100%) hue-rotate(0deg)'
  );
  check('filter also applied to canvas', s.canvas.style.filter, s.video.style.filter);

  s.vidContrast.value = 130;
  s.vidContrast.oninput();
  check(
    'contrast change reflected',
    s.video.style.filter,
    'brightness(100%) contrast(130%) saturate(100%) hue-rotate(0deg)'
  );
  check('contrast label updated', s.vidContrastV.textContent, '130%');

  s.vidFilterResetBtn.onclick();
  check(
    'reset restores defaults',
    s.video.style.filter,
    'brightness(100%) contrast(100%) saturate(100%) hue-rotate(0deg)'
  );
});

group('input is paused while the rebind panel is open', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();

  // Hold a key so there is something that WOULD be sent.
  s.kbHeldKeys['Space'] = true;
  s.recomputeKbState();
  check('key is held', s.kbState[19], 100);

  // Panel open: the state going out must be all zeros, otherwise
  // binding "A" also presses A on the console, and a button held when
  // the panel opened would stay stuck.
  s.rebindBtn.onclick();
  s.sendGamepadState();
  check('nothing is sent while the panel is open', Array.from(s.gamepadState).every((v) => v === 0), true);
  check('status says so', s.currentGamepadStatus, 'rebinding (input paused)');

  // Closing it restores control -- and it must do so even if a capture
  // was left armed, which is what previously killed the controls for
  // good.
  s.rebindListening = { kind: 'keyboard', field: 'a' };
  s.rebindCloseBtn.onclick();
  s.sendGamepadState();
  check('control returns once closed', s.gamepadState[19], 100);
  check('armed capture was cleared on close', s.rebindListening, null);
});

group('stale session token is detected', () => {
  // Tokens live only in the server's memory, so a restart invalidates
  // them while the browser keeps its copy. Without the server telling
  // the page what it actually granted, the UI would keep claiming
  // "player" while every input was silently dropped -- which is exactly
  // what happened in practice.
  const s = createSandbox(APP_JS, null, { 'capture2cloud_player_token': 'stale' });
  check('token loaded from storage', s.playerToken, 'stale');

  // Simulate the server answering "you are a viewer".
  s.playerToken = null;
  s.setPlayerUi(false);
  check('UI falls back to viewer', s.authState.textContent, 'viewer');
  check('login button offered again', s.loginBtn.classList.contains('hidden'), false);
  check('gamepad row hidden', s.gamepadSelect.parentElement.classList.contains('viewer-hidden'), true);
});

group('audio mute is shared by both routes', () => {
  const s = createSandbox(APP_JS);
  // Mute has to be one state, because the two audio routes silence
  // sound in different places: the element's own flag normally, the
  // gain node when the low-latency graph carries it.
  check('starts unmuted', s.audioMuted, false);
  s.muteBtn.onclick();
  check('mute toggles the shared state', s.audioMuted, true);
  check('element follows', s.video.muted, true);
  check('button label updated', s.muteBtn.textContent, 'unmute');
  s.muteBtn.onclick();
  check('unmute toggles back', s.audioMuted, false);
  check('element follows back', s.video.muted, false);
});

group('a viewer does not even look for a gamepad', () => {
  const s = createSandbox(APP_JS);
  let polls = 0;
  s.navigator.getGamepads = () => { polls++; return []; };

  s.setPlayerUi(false);
  check('control disabled', s.playerControlsEnabled, false);
  check('per-frame loop stopped', s.gamepadLoopId, null);

  polls = 0;
  s.updateGamepadList();
  check('dropdown refresh does not query the Gamepad API', polls, 0);
  // A pad plugged in while only watching must not start a scan either.
  s.window.dispatchGamepadConnected && s.window.dispatchGamepadConnected();
  check('still no query', polls, 0);

  s.setPlayerUi(true);
  check('control enabled', s.playerControlsEnabled, true);
  check('loop started', s.gamepadLoopId !== null, true);
  check('promotion scans for pads that appeared meanwhile', polls > 0, true);

  // Toggling back and forth must not leave two loops running.
  const id = s.gamepadLoopId;
  s.setPlayerUi(true);
  check('already running: not started twice', s.gamepadLoopId, id);
});

group('mute silences the output', () => {
  // Only one route out: the element, with a gain node bolted on top for
  // the part of the range that goes above the stream's own level.
  //
  // Turning DOWN is always the element's job and turning UP is the only
  // thing the gain does. That split is the point: the gain node lives in
  // an AudioContext the browser may leave suspended, and when the whole
  // range was delegated to it a suspended context meant a slider at zero
  // with the sound still playing.
  const s = createSandbox(APP_JS);
  s.volumeSlider.value = '50'; // half the scale = four times the stream's level
  s.ensureAudioGraph();
  check('amplification graph exists', s.gainNode !== null, true);

  s.applyVolume();
  check('amplified by the gain', s.gainNode.gain.value, 4);
  check('element carries full signal into it', s.video.volume, 1);

  s.muteBtn.onclick();
  check('mute engaged', s.audioMuted, true);
  check('element muted', s.video.muted, true);
  check('element silenced, which no suspended context can undo', s.video.volume, 0);

  s.muteBtn.onclick();
  check('unmute restores the level', s.gainNode.gain.value, 4);
  check('unmute restores the element', s.video.volume, 1);

  // Zero must be silent whether or not the graph was ever engaged, and
  // whether or not it is actually running.
  s.volumeSlider.value = '0';
  s.applyVolume();
  check('zero silences the element', s.video.volume, 0);
  // The case that was broken: past the natural level once, the graph
  // exists and owns the output, so coming back down has to silence it
  // there too.
  check('zero silences the graph that took over', s.gainNode.gain.value, 0);

  // And below the natural level the graph must attenuate, not sit at 1.
  s.volumeSlider.value = '10';
  s.applyVolume();
  check('quiet setting attenuates the graph', Math.round(s.gainNode.gain.value * 100) / 100, 0.8);
  check('quiet setting attenuates the element', Math.round(s.video.volume * 100) / 100, 0.8);
  check('element unmuted', s.video.muted, false);
});

group('controller test popup', () => {
  const s = createSandbox(APP_JS);
  s.gamepadSelect.value = s.KEYBOARD_GAMEPAD_ID;
  s.gamepadSelect.onchange();

  check('closed on load', s.padTestIsOpen(), false);
  s.padTestBtn.onclick();
  check('opens', s.padTestIsOpen(), true);

  // The three layouts must differ in more than labels: Nintendo's face
  // buttons are a diagonal mirror of Xbox's, so the same slot sits at a
  // different position.
  const xbox = s.PAD_LAYOUTS.xbox.face;
  const sw = s.PAD_LAYOUTS.switch.face;
  check('Xbox: Y on top', xbox.top.slot, 17);
  check('Switch: X on top instead', sw.top.slot, 20);
  check('Xbox: A at the bottom', xbox.bottom.slot, 19);
  check('Switch: B at the bottom', sw.bottom.slot, 18);
  check('PS5 keeps Xbox positions', s.PAD_LAYOUTS.ps5.face.top.slot, 17);
  check('PS5 relabels them', s.PAD_LAYOUTS.ps5.face.top.label, '△');

  // While testing, input must not reach the console...
  s.padTestBlock.checked = true;
  check('input suppressed while testing', s.inputIsSuppressed(), true);

  // ...but the state must still be COMPUTED, or the diagram it exists
  // to show would be blank.
  s.kbHeldKeys['Space'] = true;
  s.recomputeKbState();
  s.sendGamepadState();
  check('state still reflects the press', s.gamepadState[19], 100);

  // Opting in lets it through again.
  s.padTestBlock.checked = false;
  check('opt-out restores sending', s.inputIsSuppressed(), false);

  // The source line is redrawn 60 times a second. Building it by
  // appending to the shared status string made it grow without bound --
  // a full paragraph of "input blocked" within a second on screen.
  s.padTestBlock.checked = true; // the growth only happened while blocking
  s.updatePadTest();
  const firstLine = s.padTestSource.textContent;
  for (let i = 0; i < 60; i++) s.updatePadTest();
  check('source line does not grow when redrawn', s.padTestSource.textContent, firstLine);

  s.padTestClose.onclick();
  check('closes', s.padTestIsOpen(), false);
  check('no longer suppressing', s.inputIsSuppressed(), false);
});

group('settings persistence', () => {
  const s = createSandbox(APP_JS);
  // saveSettings must update BOTH localStorage and the live `settings`
  // object -- the bug this suite originally caught.
  s.saveSettings({ someTestKey: 42 });
  // The in-memory half is immediate: the rebind profiles are re-read
  // from `settings` at runtime and must never see stale data.
  check('in-memory settings updated', s.settings.someTestKey, 42);

  s.flushSettings();
  const stored = JSON.parse(s.localStorage.getItem('capture2cloud_settings'));
  check('localStorage updated', stored.someTestKey, 42);
  check('version stamped on save', stored.settingsVersion, s.SETTINGS_VERSION);
});

group('settings writes are coalesced', () => {
  // localStorage.setItem is synchronous and the write parses and
  // re-serialises the whole blob. Ten oninput handlers call saveSettings,
  // and oninput fires on every pixel of a slider drag -- doing the write
  // per event blocked the main thread that the gamepad loop runs on.
  const s = createSandbox(APP_JS);
  let writes = 0;
  const realSet = s.localStorage.setItem;
  s.localStorage.setItem = (k, v) => { writes++; return realSet(k, v); };

  for (let i = 0; i < 50; i++) s.saveSettings({ volume: 100 + i });
  check('a drag writes nothing yet', writes, 0);
  check('but the live value is already current', s.settings.volume, 149);

  s.flushSettings();
  check('the whole drag became one write', writes, 1);
  check('and it stored the last value', JSON.parse(s.localStorage.getItem('capture2cloud_settings')).volume, 149);

  // Every key touched during the drag has to survive, not just the last.
  s.saveSettings({ alpha: 1 });
  s.saveSettings({ beta: 2 });
  s.flushSettings();
  const blob = JSON.parse(s.localStorage.getItem('capture2cloud_settings'));
  check('first pending key kept', blob.alpha, 1);
  check('second pending key kept', blob.beta, 2);
  check('earlier value still there', blob.volume, 149);

  // A change still pending when the tab goes away must not be lost.
  writes = 0;
  s.saveSettings({ gamma: 3 });
  s.window.dispatch('pagehide');
  check('pagehide flushes the pending write', writes, 1);
  check('pending value persisted', JSON.parse(s.localStorage.getItem('capture2cloud_settings')).gamma, 3);
});

group('settings schema migration', () => {
  // Pre-versioning storage (no settingsVersion) must be kept as-is and
  // stamped, not discarded -- these are still-valid UI preferences.
  const legacy = createSandbox(APP_JS, {
    'capture2cloud_settings': JSON.stringify({ volume: 250, qualityMbps: 30, vsync: true })
  });
  // The volume scale has been rescaled twice, and a stored position goes
  // through both steps rather than being carried over -- otherwise
  // everyone's sound jumps on the next load. First 0-400 became 0-100
  // for the same loudness (250 -> 63), then the top of the scale went
  // from four times the stream's level to eight (63 -> 32).
  check('legacy volume rescaled through both changes', legacy.settings.volume, 32);
  check('legacy quality preserved', legacy.settings.qualityMbps, 30);
  check('legacy blob stamped with current version', legacy.settings.settingsVersion, legacy.SETTINGS_VERSION);

  // Storage from a hypothetical newer build must be discarded rather
  // than misread by this older code.
  const future = createSandbox(APP_JS, {
    'capture2cloud_settings': JSON.stringify({ settingsVersion: 999, volume: 400, weirdFutureKey: { a: 1 } })
  });
  check('future-version storage discarded', future.settings.volume, undefined);
  check('future-version key not carried over', future.settings.weirdFutureKey, undefined);

  // Current-version storage passes through untouched.
  // Asks the code what the current version is rather than repeating the
  // number here. Hardcoding it meant this test failed the moment the
  // version was bumped -- reporting a migration bug that did not exist,
  // and hiding whatever it was actually meant to catch.
  const current = createSandbox(APP_JS, {
    'capture2cloud_settings': JSON.stringify({
      settingsVersion: legacy.SETTINGS_VERSION, volume: 60
    })
  });
  check('current-version storage preserved', current.settings.volume, 60);

  // Corrupt JSON must not throw, just start clean.
  const corrupt = createSandbox(APP_JS, { 'capture2cloud_settings': '{not valid json' });
  check('corrupt storage falls back to empty', corrupt.settings.volume, undefined);
});

group('per-stick deadzone and range', () => {
  // The outer limit is the half that was missing. A stick that only
  // reaches 0.8 of its electrical range sends 80 when pushed all the
  // way, which the console reads as a gentle push -- so "range" says
  // what counts as fully pushed, and everything at or past it is 100.
  const s = createSandbox(APP_JS);

  s.lStickDeadzone.value = '0';
  s.lStickRange.value = '100';
  check('untouched: raw value passes through', s.stickToWire(0.5, 'left'), 50);
  check('untouched: full is full', s.stickToWire(1.0, 'left'), 100);

  s.lStickRange.value = '80';
  check('a stick reaching only 0.8 now reads as fully pushed',
    s.stickToWire(0.8, 'left'), 100);
  check('and beyond it does not overshoot', s.stickToWire(0.95, 'left'), 100);
  check('the travel below is stretched, not shifted',
    s.stickToWire(0.4, 'left'), 50);

  s.lStickDeadzone.value = '10';
  check('inside the deadzone is centred', s.stickToWire(0.08, 'left'), 0);
  check('just outside it is barely moving', s.stickToWire(0.11, 'left'), 1);
  check('negative is symmetric', s.stickToWire(-0.8, 'left'), -100);

  // A stick is round: the limits apply to the length of the vector, not
  // to each axis. Pushed fully into a corner the hardware gives about
  // 0.71 on each axis and never 1.0, so judging them separately read a
  // full diagonal as three quarters of a push -- which is what made
  // diagonals unable to reach full deflection at any threshold.
  s.lStickDeadzone.value = '0';
  s.lStickRange.value = '100';
  s.lStickDiagonal.value = '100';
  const straight = s.stickPairToWire(1.0, 0, 'left');
  check('a full push along one axis is unchanged', straight[0], 100);
  check('with nothing on the other', straight[1], 0);
  // Half way along the travel puts the DOMINANT axis at half, whatever
  // the direction -- that is what scaling by the larger component means
  // -- and the ratio between the two axes is the direction, untouched.
  const inside = s.stickPairToWire(0.3, 0.4, 'left'); // length 0.5, dominant 0.4
  check('half the travel puts the dominant axis at half', inside[1], 50);
  // Within a point: the wire carries whole numbers, so 37.5 has to
  // land on one side or the other.
  check('and the direction is preserved',
    Math.abs(inside[0] / inside[1] - 0.3 / 0.4) < 0.02, true,
    `${inside[0]}/${inside[1]}`);

  // Full deflection means the dominant axis reaches 100 whatever the
  // direction, so a corner is 100/100 and not 71/71.
  const diag = s.stickPairToWire(0.7071, 0.7071, 'left');
  check('a full diagonal reaches the maximum on both axes', diag[0], 100);
  check('and stays symmetric', diag[0], diag[1]);

  // A stick that no longer reaches into its corners: the diagonal limit
  // is what makes those corners reachable again, and it moves nothing
  // along the axes.
  s.lStickDiagonal.value = '60';
  const worn = s.stickPairToWire(0.45, 0.45, 'left'); // length 0.64
  check('a short diagonal reaches the maximum once the limit is lowered',
    worn[0], 100);
  const axisStill = s.stickPairToWire(0.64, 0, 'left');
  check('while a push along an axis is untouched by it',
    axisStill[0], 64);

  // Each stick has its own pair: the left one takes most of the wear.
  s.rStickDeadzone.value = '0';
  s.rStickRange.value = '100';
  check('the right stick is unaffected by the left one settings',
    s.stickToWire(0.5, 'right'), 50);

  // A range at or below the deadzone would divide by zero or invert.
  s.rStickDeadzone.value = '40';
  s.rStickRange.value = '50';
  const v = s.stickToWire(1.0, 'right');
  check('an impossible pair still yields a sane value', v, 100);
});

group('restarting the server', () => {
  // Refused to a viewer server-side; the button is hidden as well, but
  // hiding is a convenience and the refusal is the guard.
  const s = createSandbox(APP_JS);
  s.setPlayerUi(false);
  check('viewer: the restart button is hidden',
    s.restartServerBtn.classList.contains('viewer-hidden'), true);
  s.setPlayerUi(true);
  check('player: it is there',
    s.restartServerBtn.classList.contains('viewer-hidden'), false);

  // The socket dying IS the restart happening -- the server answers and
  // then goes -- so a failed request must lead to waiting, not to an
  // error message and a button that has given up.
  let asked = null;
  s.fetch = (url, opts) => { asked = { url, opts }; return Promise.reject(new Error('gone')); };
  s.restartServerBtn.onclick();
  check('it asks the server to restart', asked && asked.url, '/restart');
  check('and the request is a POST', asked && asked.opts.method, 'POST');
});

group('viewer/player UI state', () => {
  const s = createSandbox(APP_JS);

  // setPlayerUi is what the /auth-status response drives.
  s.setPlayerUi(false);
  check('viewer: label says viewer', s.authState.textContent, 'viewer');
  check('viewer: login button visible', s.loginBtn.classList.contains('hidden'), false);
  check('viewer: gamepad row hidden', s.gamepadSelect.parentElement.classList.contains('viewer-hidden'), true);
  check('viewer: wake console button hidden', s.wakeConsoleBtn.classList.contains('viewer-hidden'), true);

  // A viewer keeps sound controls and nothing else.
  check('viewer: mute still available', s.muteBtn.classList.contains('viewer-hidden'), false);
  check('viewer: volume still available',
    s.volumeSlider.parentElement.classList.contains('viewer-hidden'), false);
  check('viewer: controller test hidden', s.padTestBtn.classList.contains('viewer-hidden'), true);
  check('viewer: rebind hidden', s.rebindBtn.classList.contains('viewer-hidden'), true);
  // The bitrate is genuinely shared -- ONE encoder feeds every client --
  // so a viewer must not be able to degrade the player's picture.
  check('viewer: quality hidden', s.quality.parentElement.classList.contains('viewer-hidden'), true);
  check('viewer: invert stick hidden',
    s.invertRyBox.parentElement.classList.contains('viewer-hidden'), true);
  check('viewer: LT threshold hidden',
    s.ltThresholdSlider.parentElement.classList.contains('viewer-hidden'), true);
  check('viewer: brightness hidden',
    s.vidBrightness.parentElement.classList.contains('viewer-hidden'), true);
  check('viewer: vsync hidden', s.vsyncBox.parentElement.classList.contains('viewer-hidden'), true);

  s.setPlayerUi(true);
  check('player: label says player', s.authState.textContent, 'player');
  check('player: login button hidden', s.loginBtn.classList.contains('hidden'), true);
  check('player: gamepad row visible', s.gamepadSelect.parentElement.classList.contains('viewer-hidden'), false);
  check('player: wake console button visible', s.wakeConsoleBtn.classList.contains('viewer-hidden'), false);
  check('player: controller test back', s.padTestBtn.classList.contains('viewer-hidden'), false);
  check('player: quality back', s.quality.parentElement.classList.contains('viewer-hidden'), false);

  // Being demoted must not leave a gamepad panel open on screen.
  s.padTestBtn.onclick();
  check('test popup open as player', s.padTestIsOpen(), true);
  s.setPlayerUi(false);
  check('test popup closed when demoted', s.padTestIsOpen(), false);
  s.setPlayerUi(true);

  // Dropping back to viewer must also tear down the virtual overlay,
  // not leave it on screen sending input the server would ignore.
  s.gamepadSelect.value = s.VIRTUAL_GAMEPAD_ID;
  s.gamepadSelect.onchange();
  check('overlay shown while player', s.vgpEl.classList.contains('hidden'), false);
  s.setPlayerUi(false);
  check('overlay hidden when dropping to viewer', s.vgpEl.classList.contains('hidden'), true);
});

group('player-gated requests carry the token', () => {
  // Forgetting the header is invisible in the browser -- the server
  // answers 403 and the page just does nothing. It happened on /quality
  // and /capture-format at once, which made the quality slider inert and
  // made the capture toggle snap back (the read-back afterwards
  // correctly reported that nothing had changed).
  const s = createSandbox(APP_JS, null, { 'capture2cloud_player_token': 'cafebabe' });
  s.setPlayerUi(true);
  s.fetchCalls.length = 0;

  const tokenOf = (call) => (call.options.headers || {})['X-Player-Token'];

  s.quality.value = '20';
  s.sendQuality();
  // The POST is debounced, so it only leaves once the timer fires.
  s.runPendingTimers();
  const q = s.fetchCalls.find((c) => c.url === '/quality');
  check('/quality was called', !!q, true);
  check('/quality carries the token', q && tokenOf(q), 'cafebabe');

  s.captureFormatSelect.onchange({ target: { value: 'mjpeg' } });
  const f = s.fetchCalls.find((c) => c.url === '/capture-format' && c.options.method === 'POST');
  check('/capture-format was called', !!f, true);
  check('/capture-format carries the token', f && tokenOf(f), 'cafebabe');

  s.wakeConsoleBtn.onclick();
  const w = s.fetchCalls.find((c) => c.url === '/wake');
  check('/wake carries the token', w && tokenOf(w), 'cafebabe');

  // A viewer has no token; the header must simply be absent, not empty.
  const v = createSandbox(APP_JS);
  v.fetchCalls.length = 0;
  v.playerFetch('/quality', { method: 'POST', body: '1' }).catch(() => {});
  check('viewer sends no token header',
    'X-Player-Token' in (v.fetchCalls[0].options.headers || {}), false);
});

group('capture format choice is remembered', () => {
  const s = createSandbox(APP_JS, null, { 'capture2cloud_player_token': 'cafebabe' });
  s.setPlayerUi(true);
  s.captureFormatSelect.onchange({ target: { value: 'mjpeg' } });
  s.flushSettings();
  check('the choice is stored', s.settings.captureFormat, 'mjpeg');
  check('and persisted',
    JSON.parse(s.localStorage.getItem('capture2cloud_settings')).captureFormat, 'mjpeg');

  // A fresh page with that stored choice must NOT push it. There is one
  // capture device: loading a page used to reconfigure the card for
  // everyone already watching, to whatever this browser had saved.
  const s2 = createSandbox(APP_JS,
    { 'capture2cloud_settings': JSON.stringify({ settingsVersion: 2, captureFormat: 'mjpeg' }) },
    { 'capture2cloud_player_token': 'cafebabe' });
  s2.fetchCalls.length = 0;
  s2.setPlayerUi(true);
  check('a player pushes nothing on load',
    !!s2.fetchCalls.find((c) => c.url === '/capture-format' && c.options.method === 'POST'), false);

  // A viewer must never push a format: it is one shared device.
  const v = createSandbox(APP_JS,
    { 'capture2cloud_settings': JSON.stringify({ settingsVersion: 2, captureFormat: 'mjpeg' }) });
  v.fetchCalls.length = 0;
  v.setPlayerUi(false);
  check('a viewer pushes nothing',
    !!v.fetchCalls.find((c) => c.url === '/capture-format' && c.options.method === 'POST'), false);
});

group('shared settings are followed, not imposed', () => {
  // Everything here exists because one encoder and one capture card
  // serve every client: a page that pushes its own saved values on load
  // changes the picture for people who were already watching.
  const s = createSandbox(APP_JS,
    { 'capture2cloud_settings': JSON.stringify({ settingsVersion: 4, qualityMbps: 40 }) },
    { 'capture2cloud_player_token': 'cafebabe' });
  s.setPlayerUi(true);
  check('the saved bitrate is not posted on load',
    !!s.fetchCalls.find((c) => c.url === '/quality' && c.options.method === 'POST'), false);
  check('the shared state is asked for',
    !!s.fetchCalls.find((c) => c.url === '/shared'), true);

  // What the host says wins over what this page had saved.
  s.applyShared({ height: 720, bitrate_kbps: 6000, capture: 'mjpeg' });
  check('the resolution follows', s.resolutionSelect.value, '720');
  check('the bitrate follows', s.quality.value, '6');
  check('the label follows', s.qv.textContent, '6 Mbps');
  check('the capture format follows', s.captureFormatSelect.value, 'mjpeg');

  // ...and following it must not bounce back, or two open pages would
  // correct each other for ever.
  check('following posts nothing',
    !!s.fetchCalls.find((c) => c.options && c.options.method === 'POST'), false);

  // A setting the user is still holding is not yanked back by a reply
  // that was already in flight.
  s.quality.value = '20';
  s.sendQuality();
  s.applyShared({ height: 480, bitrate_kbps: 2000, capture: 'yuyv' });
  check('a just-touched control is left alone', s.quality.value, '20');
});

group('every place that names a version names the same one', () => {
  // Four files say which release this is: VERSION, version.h (the host
  // and the console client), web/main.js (the page) and the Android
  // build. They are separate because they are read by four different
  // toolchains, which makes a bump that misses one both easy and
  // invisible -- the symptom is a client confidently reporting a
  // version nobody shipped.
  const root = path.join(__dirname, '..');
  const version = fs.readFileSync(path.join(root, 'VERSION'), 'utf8').trim();
  check('VERSION looks like a release', /^\d+\.\d+\.\d+$/.test(version), true);

  const fromHeader = /#define C2C_VERSION "([^"]+)"/.exec(
    fs.readFileSync(path.join(root, 'version.h'), 'utf8'));
  check('version.h matches VERSION', fromHeader && fromHeader[1], version);

  const fromPage = /var C2C_VERSION = '([^']+)'/.exec(
    fs.readFileSync(path.join(root, 'web', 'main.js'), 'utf8'));
  check('the page matches VERSION', fromPage && fromPage[1], version);

  const fromGradle = /versionName = "([^"]+)"/.exec(
    fs.readFileSync(path.join(root, 'android', 'app', 'build.gradle.kts'), 'utf8'));
  check('the Android build matches VERSION', fromGradle && fromGradle[1], version);
});

group('the page and the test suite agree on what to load', () => {
  // The front end is ten plain scripts sharing one scope, so the ORDER
  // is part of the program: a function declaration hoists within a file
  // and not across two. This suite runs them concatenated, which means
  // a file added to page.html and not to WEB_FILES (or the reverse)
  // would be tested in an order the browser never uses -- green here,
  // broken there. So the two lists are compared.
  const html = fs.readFileSync(path.join(__dirname, '..', 'page.html'), 'utf8');
  const inPage = [];
  const re = /<script src="\/web\/([^"]+)"><\/script>/g;
  let m;
  while ((m = re.exec(html))) inPage.push(m[1]);
  check('page.html loads the same files, in the same order',
    inPage.join(','), WEB_FILES.join(','));

  // Every one of them must exist, since a 404 on a script tag is silent
  // in the browser: the page simply behaves as though that file's half
  // of the program was never written.
  const missing = WEB_FILES.filter(
    (f) => !fs.existsSync(path.join(__dirname, '..', 'web', f)));
  check('every file listed exists', missing.join(','), '');
});

group('player token storage', () => {
  // A token left over from a previous page load is picked up on start.
  const s = createSandbox(APP_JS, null, { 'capture2cloud_player_token': 'deadbeef' });
  check('token restored from sessionStorage', s.playerToken, 'deadbeef');

  // ...and it must NOT be written into the persisted settings blob.
  const stored = s.localStorage.getItem('capture2cloud_settings');
  check('token absent from settings blob', stored === null || stored.indexOf('deadbeef') === -1, true);
});

console.log(`\n${passed} passed, ${failed} failed`);
if (failed > 0) {
  console.log('\nFailures:');
  failures.forEach((f) => console.log(`  - ${f}`));
  process.exit(1);
}
