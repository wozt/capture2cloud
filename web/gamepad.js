/*
 * capture2cloud -- real controllers, and the wire they end up on
 *
 * The browser's Gamepad API, per-stick shaping, the per-controller
 * rebind profiles, and the loop that merges every source of input into
 * the 21 bytes the host expects.
 *
 * Loaded before touchpad.js and keyboard.js: both add themselves to
 * the merge this file performs, and both call into the shaping here.
 */

/* --- gamepad: browser Gamepad API -> DataChannel -> Titan One.
 * Order of the 21 bytes sent = the GAMEPAD_XB360_* entries from
 * gamepad_bridge.h on the server side (signed values, -100..100). The
 * server does no transformation at all: all the mapping happens here.
 * Unreliable/unordered channel (like UDP): this is a current state sent
 * continuously, no point retransmitting a stale value. */
var gamepadChannel = null;
var gamepadState = new Int8Array(21);
var selectedGamepadId = settings.gamepadId || null;
var currentGamepadStatus = 'none';

/* In "Virtual buttons" mode, a real gamepad plugged in and detected by
 * the browser stays active too, combined with the touch overlay -- so
 * touching a button and pressing the real one both work, and the
 * on-screen buttons/sticks visually reflect the real controller's input
 * even without any touch. Touch handlers write to vgpTouchState (never
 * directly to gamepadState); the per-frame combiner in sendGamepadState()
 * adds a freshly-read real-gamepad state on top before sending. */
var vgpTouchState = new Int8Array(21);

/* Sent in place of the real state while input is suppressed. Kept
 * separate rather than zeroing `gamepadState`, because the controller
 * test popup has to keep DISPLAYING what would be sent while nothing
 * actually reaches the console -- zeroing the state would blank the
 * very diagram the popup exists to show. */
var zeroState = new Int8Array(21);

/* True when input must not reach the console: while testing buttons
 * (unless explicitly allowed) -- pressing everything to check it
 * shouldn't also press it in the running game. */
function inputIsSuppressed() {
  return padTestIsOpen() && padTestBlock.checked;
}

function sendGamepadBuffer() {
  if (!gamepadChannel || gamepadChannel.readyState !== 'open') return;
  gamepadChannel.send(inputIsSuppressed() ? zeroState.buffer : gamepadState.buffer);
}

/* "Keyboard/mouse" mode's own state, declared here (rather than down by
 * the rest of its logic) so it already exists by the time the initial
 * mode-activation calls run further below -- kbState/kbHeldKeys are
 * filled in by keydown/keyup handlers, mouseStickX/Y by mousemove while
 * pointer-locked. See the "Keyboard/mouse mode" section for the rest. */
var kbState = new Int8Array(21);
var kbHeldKeys = {};
var mouseStickX = 0,
  mouseStickY = 0;

/* Rebind panel DOM refs + listening state -- declared here (not down by
 * the rest of the rebind panel logic) for the same reason as the
 * keyboard state above: gamepadSelect's init/onchange code below needs
 * them to already exist. */
var rebindEl = document.getElementById('rebind');
var rebindBtn = document.getElementById('rebind-btn');
var rebindTitle = document.getElementById('rebind-title');
var rebindProfileRow = document.getElementById('rebind-profile-row');
var rebindRowsEl = document.getElementById('rebind-rows');
var rebindNote = document.getElementById('rebind-note');
var rebindCloseBtn = document.getElementById('rebind-close');
var rebindListening = null; // { kind: 'gamepad'|'keyboard', field }
var rebindGpSnapshot = null;

/* Up/down inversion of the right stick, on the fly -- observed inverted
 * on some gamepads/console-side translations. */
invertRyBox.checked = settings.invertRy === true;
invertRyBox.onchange = function (e) {
  saveSettings({ invertRy: e.target.checked });
};

/* Adjustable trigger thresholds: on some gamepads (Xbox Series X|S
 * observed here), LT/RT do not come back as standard buttons but as
 * extra AXES (4 and 5), on a range from -1.00 (released) to 1.00 (fully
 * pressed) -- hence a threshold, not a simple boolean. The right value
 * can vary from one gamepad to another, hence the on-the-fly setting
 * rather than a fixed constant. */
ltThresholdSlider.value = settings.ltThreshold != null ? settings.ltThreshold : 0.3;
rtThresholdSlider.value = settings.rtThreshold != null ? settings.rtThreshold : 0.3;
ltv.textContent = Number(ltThresholdSlider.value).toFixed(2);
rtv.textContent = Number(rtThresholdSlider.value).toFixed(2);
ltThresholdSlider.oninput = function () {
  ltv.textContent = Number(ltThresholdSlider.value).toFixed(2);
  saveSettings({ ltThreshold: Number(ltThresholdSlider.value) });
};
rtThresholdSlider.oninput = function () {
  rtv.textContent = Number(rtThresholdSlider.value).toFixed(2);
  saveSettings({ rtThreshold: Number(rtThresholdSlider.value) });
};

/* --- per-stick limits ------------------------------------------------
 *
 * Two numbers per stick, and the outer one is the important half.
 *
 * A stick does not reach its electrical maximum, least of all on a
 * diagonal, so pushing it all the way sends 0.8 rather than 1.0 and the
 * console reads a gentle push. Setting the range to what the stick
 * actually reaches makes the end of its travel mean full deflection
 * again, and the travel in between is stretched across the whole scale
 * so the middle stays proportional rather than jumping.
 *
 * Per stick because the two wear at different rates: the left one takes
 * most of the movement in most games and goes first. */
var stickLimits = {
  left: { deadzone: lStickDeadzone, range: lStickRange, diagonal: lStickDiagonal },
  right: { deadzone: rStickDeadzone, range: rStickRange, diagonal: rStickDiagonal }
};

function bindStickLimit(slider, label, key, fallback) {
  var out = document.getElementById(label);
  slider.value = settings[key] != null ? settings[key] : fallback;
  out.textContent = slider.value + '%';
  slider.oninput = function () {
    out.textContent = slider.value + '%';
    var change = {};
    change[key] = Number(slider.value);
    saveSettings(change);
  };
}
bindStickLimit(lStickDeadzone, 'lstick-deadzone-v', 'lStickDeadzone', 5);
bindStickLimit(lStickRange, 'lstick-range-v', 'lStickRange', 100);
bindStickLimit(rStickDeadzone, 'rstick-deadzone-v', 'rStickDeadzone', 5);
bindStickLimit(rStickRange, 'rstick-range-v', 'rStickRange', 100);
bindStickLimit(lStickDiagonal, 'lstick-diagonal-v', 'lStickDiagonal', 100);
bindStickLimit(rStickDiagonal, 'rstick-diagonal-v', 'rStickDiagonal', 100);

/* Both axes of one stick, from the Gamepad API's -1..1 to the wire's
 * -100..100, with that stick's two limits applied.
 *
 * The limits apply to the LENGTH of the vector, not to each axis on its
 * own, because a stick is round. The hardware limits the two together to
 * a circle, so pushing fully into a corner gives about 0.71 on each axis
 * and never 1.0; judged per axis that reads as three quarters of a push,
 * which is why a diagonal would not reach full deflection even with the
 * range set to 90%. Scaling the vector and keeping its direction means a
 * stick pushed as far as it goes is full deflection wherever it is
 * pointed. */
function stickPairToWire(rawX, rawY, which) {
  var limits = stickLimits[which];
  var dead = Number(limits.deadzone.value) / 100;
  var satAxis = Number(limits.range.value) / 100;
  var satDiag = Number(limits.diagonal.value) / 100;

  var mag = Math.sqrt(rawX * rawX + rawY * rawY);
  if (mag <= dead || mag < 1e-6) return [0, 0];

  var ax = Math.abs(rawX), ay = Math.abs(rawY);
  var peak = Math.max(ax, ay);
  /* 0 straight along an axis, 1 at exactly 45 degrees. The two outer
   * limits blend by this, so there is no seam anywhere in the circle. */
  var diagonality = peak > 1e-6 ? Math.min(ax, ay) / peak : 0;
  var sat = satAxis + (satDiag - satAxis) * diagonality;
  if (sat <= dead) sat = dead + 0.1;

  /* How far along the usable travel, 0 at the deadzone and 1 at the
   * outer limit, so the middle stays proportional rather than jumping. */
  var t = Math.min(1, (mag - dead) / (sat - dead));
  /* Scaled by the LARGER component, not by the length: at the outer
   * limit the dominant axis is exactly 100 whatever the direction, so a
   * corner comes out as 100/100 rather than 71/71 -- which is what "as
   * far as it goes" should mean for something handed to a game. */
  var scale = (t * 100) / peak;
  var clamp = function (v) { return Math.max(-100, Math.min(100, Math.round(v))); };
  return [clamp(rawX * scale), clamp(rawY * scale)];
}

/* One axis, for anything that only has one. Same rule: with the other
 * axis at rest the vector's length is this axis's own value. */
function stickToWire(raw, which) {
  return stickPairToWire(raw, 0, which)[0];
}

function clamp100(v) {
  return Math.max(-100, Math.min(100, Math.round(v * 100)));
}


/* --- per-real-gamepad rebind profiles: which browser button/axis index
 * feeds each GAMEPAD_XB360_* slot, stored per gamepad id
 * (settings.gamepadProfiles[gp.id]) since different controllers need
 * different treatment -- confirmed this session with the Xbox Series
 * X|S controller's own nonstandard browser button order, and expected
 * again with a PS5 DualSense (different, but standard-compliant,
 * order). defaultGamepadProfile() is exactly the mapping this file used
 * to have hardcoded, so a controller that's never been rebound behaves
 * exactly as before. */
var GAMEPAD_ROWS = [
  { field: 'guide', label: 'Guide', kind: 'button' },
  { field: 'back', label: 'Back', kind: 'button' },
  { field: 'start', label: 'Start', kind: 'button' },
  { field: 'rb', label: 'RB', kind: 'button' },
  { field: 'rt', label: 'RT', kind: 'trigger' },
  { field: 'rs', label: 'RS', kind: 'button' },
  { field: 'lb', label: 'LB', kind: 'button' },
  { field: 'lt', label: 'LT', kind: 'trigger' },
  { field: 'ls', label: 'LS', kind: 'button' },
  { field: 'rx', label: 'RX', kind: 'axis' },
  { field: 'ry', label: 'RY', kind: 'axis' },
  { field: 'lx', label: 'LX', kind: 'axis' },
  { field: 'ly', label: 'LY', kind: 'axis' },
  { field: 'up', label: 'Up', kind: 'button' },
  { field: 'down', label: 'Down', kind: 'button' },
  { field: 'left', label: 'Left', kind: 'button' },
  { field: 'right', label: 'Right', kind: 'button' },
  { field: 'y', label: 'Y', kind: 'button' },
  { field: 'b', label: 'B', kind: 'button' },
  { field: 'a', label: 'A', kind: 'button' },
  { field: 'x', label: 'X', kind: 'button' }
];

/* Whether this controller reports its top/left face buttons transposed.
 *
 * This was originally applied to every gamepad, on the belief that the
 * swap happened in the GCAPI translation to the console. It does not:
 * it is the controller misreporting itself. The Xbox Series X|S pad here
 * comes back with 18 buttons and 6 axes instead of the standard 17/4,
 * with X and Y transposed -- while a PS5 DualSense reports a proper
 * standard mapping, so compensating for it there is what inverted square
 * and triangle. Keyed on the controller so both are right untouched;
 * anything this guesses wrong stays fixable per-pad in the rebind panel.
 *
 * The underlying cause is likely the Linux xpad driver's button order,
 * so the same pad on another OS may well report standard mapping and not
 * want this. That case rebinds like any other. */
function gamepadNeedsXYSwap(gpId) {
  var id = String(gpId || '').toLowerCase();
  return id.indexOf('045e') !== -1 /* Microsoft vendor id */ || /xbox|xinput/.test(id);
}

function defaultGamepadProfile(gpId) {
  var swapXY = gamepadNeedsXYSwap(gpId);
  return {
    guide: { type: 'button', index: 16 },
    back: { type: 'button', index: 8 },
    start: { type: 'button', index: 9 },
    rb: { type: 'button', index: 5 },
    rt: { type: 'axis', index: 5, fallbackButton: 7 },
    rs: { type: 'button', index: 11 },
    lb: { type: 'button', index: 4 },
    lt: { type: 'axis', index: 4, fallbackButton: 6 },
    ls: { type: 'button', index: 10 },
    rx: { type: 'axis', index: 2 },
    ry: { type: 'axis', index: 3 },
    lx: { type: 'axis', index: 0 },
    ly: { type: 'axis', index: 1 },
    up: { type: 'button', index: 12 },
    down: { type: 'button', index: 13 },
    left: { type: 'button', index: 14 },
    right: { type: 'button', index: 15 },
    /* Standard mapping is 0=bottom, 1=right, 2=left, 3=top. Only pads
     * that transpose the two get them the other way round. */
    y: { type: 'button', index: swapXY ? 2 : 3 },
    b: { type: 'button', index: 1 },
    a: { type: 'button', index: 0 },
    x: { type: 'button', index: swapXY ? 3 : 2 }
  };
}
/* Defaults are immutable and depend only on the id, so they are built
 * once per controller rather than on every frame. buildStateFromGamepad()
 * asks for a profile 60 times a second, and a gamepad that has never been
 * rebound -- the normal case -- took the defaults branch every time,
 * allocating 22 objects plus a lowercased copy of the id for the
 * work of reading four button indices. */
var defaultProfileCache = {};

function getGamepadProfile(gpId) {
  var profiles = settings.gamepadProfiles || {};
  if (profiles[gpId]) return profiles[gpId];
  if (!defaultProfileCache[gpId]) {
    defaultProfileCache[gpId] = defaultGamepadProfile(gpId);
  }
  return defaultProfileCache[gpId];
}
function saveGamepadProfile(gpId, profile) {
  var profiles = settings.gamepadProfiles || {};
  profiles[gpId] = profile;
  saveSettings({ gamepadProfiles: profiles });
}

/* Builds the 21-value GAMEPAD_XB360_* state from a real browser Gamepad
 * object into `out` (any 21-length indexable, zeroed by the caller
 * first if a clean base is needed), using that gamepad's own rebind
 * profile. Shared by the normal "pick one real gamepad from the
 * dropdown" path, "Virtual buttons" mode's real-gamepad-stays-active
 * merge, and "Keyboard/mouse" mode's real-gamepad merge -- all three
 * behave identically (same invert-ry/threshold settings, same per-
 * gamepad rebind profile). */
function buildStateFromGamepad(gp, out) {
  var b = gp.buttons;
  var a = gp.axes;
  var profile = getGamepadProfile(gp.id);
  var digitalField = function (name) {
    var f = profile[name];
    return f && b[f.index] && b[f.index].pressed ? 100 : 0;
  };
  /* Read as a pair, because the limits apply to the vector rather than
   * to each axis: see stickPairToWire. */
  var stickPair = function (xName, yName, which) {
    var fx = profile[xName], fy = profile[yName];
    var rx = fx && a[fx.index] !== undefined ? a[fx.index] : 0;
    var ry = fy && a[fy.index] !== undefined ? a[fy.index] : 0;
    return stickPairToWire(rx, ry, which);
  };
  /* Triggers: on some gamepads (Xbox Series X|S observed here), LT/RT
   * do NOT come back as standard buttons but as extra axes (range
   * -1.00 to 1.00, 1.00 = fully pressed) -- read via the bound axis
   * first, thresholded by the adjustable sliders on the site, falling
   * back to a plain button read if the binding is a button instead
   * (either the profile's own fallbackButton, or a button the user
   * explicitly rebound this field to via the rebind panel). */
  var triggerField = function (name, threshold) {
    var f = profile[name];
    if (!f) return 0;
    if (f.type === 'axis' && a[f.index] !== undefined) {
      return a[f.index] > threshold ? 100 : 0;
    }
    var buttonIndex = f.type === 'button' ? f.index : f.fallbackButton;
    return buttonIndex != null && b[buttonIndex] && (b[buttonIndex].pressed || b[buttonIndex].value > 0.9) ? 100 : 0;
  };

  out[0] = digitalField('guide');
  out[1] = digitalField('back');
  out[2] = digitalField('start');
  out[3] = digitalField('rb');
  out[4] = triggerField('rt', Number(rtThresholdSlider.value));
  out[5] = digitalField('rs');
  out[6] = digitalField('lb');
  out[7] = triggerField('lt', Number(ltThresholdSlider.value));
  out[8] = digitalField('ls');
  var right = stickPair('rx', 'ry', 'right');
  var left = stickPair('lx', 'ly', 'left');
  out[9] = right[0];
  out[10] = invertRyBox.checked ? -right[1] : right[1];
  out[11] = left[0];
  out[12] = left[1];
  out[13] = digitalField('up');
  out[14] = digitalField('down');
  out[15] = digitalField('left');
  out[16] = digitalField('right');
  out[17] = digitalField('y');
  out[18] = digitalField('b');
  out[19] = digitalField('a');
  out[20] = digitalField('x');
}

var vgpRealState = new Int8Array(21);

/* Positions a stick's knob from a -100..100 value pair, used to reflect
 * a real gamepad's axis position on-screen while no finger is on that
 * particular stick (a finger, when present, already positions the knob
 * itself in bindVgpStick's own update()). */
function setVgpKnobFromValue(key, vx, vy) {
  var s = vgpStickEls[key];
  if (!s) return;
  var maxR = s.el.getBoundingClientRect().width / 2;
  s.knob.style.transform = 'translate(' + (vx / 100) * maxR + 'px,' + (vy / 100) * maxR + 'px)';
}

/* Combines touch input with a real gamepad's input (if any is connected)
 * into `gamepadState`, and keeps the overlay's own visuals (pressed
 * buttons, stick knob positions) in sync with whichever source is
 * driving them -- called once per frame, only in "Virtual buttons"
 * mode. Saturating add: either source alone passes through unchanged,
 * both together add up and clamp, same philosophy as the (server-side,
 * currently disabled) real-controller passthrough merge. */
function combineVirtualGamepadState() {
  var realGp = findAnyRealGamepad();
  vgpRealState.fill(0);
  if (realGp) {
    var settling =
      gamepadConnectedAt[realGp.id] && performance.now() - gamepadConnectedAt[realGp.id] < GAMEPAD_SETTLE_MS;
    if (!settling) buildStateFromGamepad(realGp, vgpRealState);
  }
  for (var i = 0; i < 21; i++) {
    gamepadState[i] = Math.max(-100, Math.min(100, vgpTouchState[i] + vgpRealState[i]));
  }
  VGP_DIGITAL_BINDINGS.forEach(function (bnd) {
    bnd.el.classList.toggle('pressed', gamepadState[bnd.idx] !== 0);
  });
  if (vgpDpadArrows) {
    vgpDpadArrows.up.classList.toggle('active', gamepadState[13] !== 0);
    vgpDpadArrows.down.classList.toggle('active', gamepadState[14] !== 0);
    vgpDpadArrows.left.classList.toggle('active', gamepadState[15] !== 0);
    vgpDpadArrows.right.classList.toggle('active', gamepadState[16] !== 0);
  }
  if (!vgpStickActive.l) setVgpKnobFromValue('l', gamepadState[11], gamepadState[12]);
  if (!vgpStickActive.r) setVgpKnobFromValue('r', gamepadState[9], gamepadState[10]);
  return realGp;
}


function combineKeyboardGamepadState() {
  updateMouseStick();
  var realGp = findAnyRealGamepad();
  vgpRealState.fill(0);
  if (realGp) {
    var settling =
      gamepadConnectedAt[realGp.id] && performance.now() - gamepadConnectedAt[realGp.id] < GAMEPAD_SETTLE_MS;
    if (!settling) buildStateFromGamepad(realGp, vgpRealState);
  }
  for (var i = 0; i < 21; i++) {
    gamepadState[i] = Math.max(-100, Math.min(100, kbState[i] + vgpRealState[i]));
  }
  gamepadState[9] = Math.max(-100, Math.min(100, gamepadState[9] + Math.round(mouseStickX)));
  gamepadState[10] = Math.max(-100, Math.min(100, gamepadState[10] + Math.round(mouseStickY)));
  return realGp;
}

function sendGamepadState() {
  /* While the rebind panel is open, every press is meant for it --
   * forwarding them would also fire them in the game, so mapping "A"
   * would press whatever A currently is on the console.
   *
   * Gated on the panel being OPEN, not on a capture being in progress:
   * clicking "listen" and then doing something else leaves the capture
   * armed, and gating on that left the controls dead until the panel
   * was closed. Closing the panel now always restores control.
   *
   * A zeroed state is sent rather than nothing at all: a button held
   * when the panel opened has to be released, not left stuck at its
   * last value. */
  if (!rebindEl.classList.contains('hidden')) {
    gamepadState.fill(0);
    sendGamepadBuffer();
    currentGamepadStatus = 'rebinding (input paused)';
    return;
  }
  if (selectedGamepadId === VIRTUAL_GAMEPAD_ID) {
    var realGp = combineVirtualGamepadState();
    currentGamepadStatus =
      'virtual buttons (touch, ' + vgpLayoutSelect.value + ' layout)' + (realGp ? ' + ' + realGp.id : '');
    if (!gpDebugEl.classList.contains('hidden')) {
      gpDebugEl.textContent = 'virtual[' + Array.prototype.join.call(gamepadState, ' ') + ']';
    }
    sendGamepadBuffer();
    return;
  }
  if (selectedGamepadId === KEYBOARD_GAMEPAD_ID) {
    var kbRealGp = combineKeyboardGamepadState();
    /* Say plainly how to get the cursor back: while the pointer is
     * locked the settings bar cannot be reached at all, and nothing else
     * on screen hints that Escape is the way out. */
    currentGamepadStatus =
      'keyboard/mouse (' + getActiveKeyboardProfileName() + ')' +
      (mouseIsPlaying() ? ' [Esc frees the cursor]' : ' [click the video to capture the mouse]') +
      (kbRealGp ? ' + ' + kbRealGp.id : '');
    if (!gpDebugEl.classList.contains('hidden')) {
      gpDebugEl.textContent = 'keyboard[' + Array.prototype.join.call(gamepadState, ' ') + ']';
    }
    sendGamepadBuffer();
    return;
  }
  var gp = getSelectedGamepad();
  currentGamepadStatus = gp
    ? gp.id + ' (mapping:' + (gp.mapping || 'none') + ' b:' + gp.buttons.length + ' a:' + gp.axes.length + ')'
    : selectedGamepadId
      ? 'disconnected (' + selectedGamepadId + ')'
      : 'none';
  /* Raw dump of the browser's buttons/axes: used to diagnose non-standard
   * gamepads or indices that don't match what we assume (e.g. a "select"
   * button that appears permanently pressed). */
  /* Only worth building when someone can actually read it. This runs 60
   * times a second: two map+join passes, a toFixed() per axis and a DOM
   * write, all of it discarded while the stats row is collapsed -- which
   * is its default state. */
  if (!gpDebugEl.classList.contains('hidden')) {
    if (gp) {
      var btnDump = gp.buttons
        .map(function (btn, i) {
          return i + ':' + (btn.pressed ? '1' : '0');
        })
        .join(' ');
      var axDump = gp.axes
        .map(function (v, i) {
          return i + ':' + v.toFixed(2);
        })
        .join(' ');
      gpDebugEl.textContent = 'buttons[' + btnDump + '] axes[' + axDump + ']';
    } else {
      gpDebugEl.textContent = '';
    }
  }
  if (!gamepadChannel || gamepadChannel.readyState !== 'open') return;
  var s = gamepadState;
  s.fill(0);
  var settling =
    gp && gamepadConnectedAt[gp.id] && performance.now() - gamepadConnectedAt[gp.id] < GAMEPAD_SETTLE_MS;
  if (gp && !settling) {
    buildStateFromGamepad(gp, s);
  }
  sendGamepadBuffer();
}

/* Only runs for a player. A viewer has nothing to send, so the whole
 * loop -- polling the Gamepad API every frame, reading the keyboard and
 * mouse state, touching the DataChannel -- is stopped outright rather
 * than run and thrown away. Restarted the moment a login succeeds. */
var gamepadLoopId = null;

function setGamepadLoopRunning(on) {
  /* Loose comparison on purpose: this var is declared further down the
   * file, so an early caller would see `undefined` rather than null and
   * a strict check would quietly refuse to start the loop. */
  if (on && gamepadLoopId == null) {
    gamepadLoopId = requestAnimationFrame(gamepadLoop);
  } else if (!on && gamepadLoopId != null) {
    cancelAnimationFrame(gamepadLoopId);
    gamepadLoopId = null;
  }
}

function gamepadLoop() {
  pollGamepadRebind();
  sendGamepadState();
  if (padTestIsOpen()) {
    /* After sendGamepadState(), so the diagram shows exactly the values
     * that WOULD be sent: while blocking, zeros go on the wire but
     * `gamepadState` still holds the real reading, which is what the
     * diagram must show. */
    updatePadTest();
  }
  gamepadLoopId = requestAnimationFrame(gamepadLoop);
}
