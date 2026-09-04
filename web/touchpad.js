/*
 * capture2cloud -- the on-screen pad
 *
 * A touch overlay for phones and tablets: face buttons, shoulders, two
 * sticks, a real cross with diagonals, and a mode that lets every one
 * of them be dragged somewhere better and remembered there.
 */

/* --- virtual on-screen gamepad: touch overlay (e.g. for Android), a
 * fixed extra entry in the same dropdown as real gamepads. Drives
 * `gamepadState` directly from touch input instead of polling
 * navigator.getGamepads() -- Pointer Events are used throughout (not
 * touch events) because each button/stick captures its own pointer via
 * setPointerCapture(): several fingers held on different controls at
 * once (a stick + several buttons) are naturally independent, no shared
 * touch-list bookkeeping needed, and a finger sliding slightly off a
 * button while held stays captured by it instead of leaking onto a
 * neighboring control. */
var VIRTUAL_GAMEPAD_ID = '__virtual__';
var KEYBOARD_GAMEPAD_ID = '__keyboard__';
var vgpEl = document.getElementById('vgp');
var vgpLayoutRow = document.getElementById('vgp-layout-row');
var vgpLayoutSelect = document.getElementById('vgp-layout');
var vgpSymmetricRow = document.getElementById('vgp-symmetric-row');
var vgpSymmetricBox = document.getElementById('vgp-symmetric');
var vgpEditRow = document.getElementById('vgp-edit-row');
var vgpEditBox = document.getElementById('vgp-edit-toggle');
var vgpResetBtn = document.getElementById('vgp-reset');
var vgpColorRow = document.getElementById('vgp-color-row');
var vgpColorInput = document.getElementById('vgp-color');
var vgpOpacityRow = document.getElementById('vgp-opacity-row');
var vgpOpacityInput = document.getElementById('vgp-opacity');
var vgpOpacityV = document.getElementById('vgp-opacity-v');

/* Overall opacity of the whole overlay (buttons, sticks, d-pad, labels --
 * everything), so it can be faded down to see more of the game through
 * it rather than only ever the fixed ~14% backgrounds baked into the
 * CSS. Applied as a plain CSS opacity on the #vgp container: simplest
 * way to fade every element uniformly together in one go. */
function applyVgpOpacity(pct) {
  vgpEl.style.opacity = String(pct / 100);
  vgpOpacityV.textContent = pct + '%';
}
vgpOpacityInput.value = settings.vgpOpacity != null ? settings.vgpOpacity : 100;
applyVgpOpacity(Number(vgpOpacityInput.value));
vgpOpacityInput.oninput = function () {
  saveSettings({ vgpOpacity: Number(vgpOpacityInput.value) });
  applyVgpOpacity(Number(vgpOpacityInput.value));
};

/* Custom accent color for the neutral controls (sticks, d-pad, shoulder/
 * trigger buttons, start/select/guide, L3/R3) -- NOT the face buttons,
 * which keep their brand-skin colors (recoloring those would defeat the
 * point of picking a layout). Applied as CSS variables on #vgp so a
 * single color choice tints background/border/pressed-state/knob
 * consistently everywhere via plain CSS, no per-element rewriting. */
function hexToRgba(hex, alpha) {
  var m = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex || '');
  var r = m ? parseInt(m[1], 16) : 255;
  var g = m ? parseInt(m[2], 16) : 255;
  var b = m ? parseInt(m[3], 16) : 255;
  return 'rgba(' + r + ',' + g + ',' + b + ',' + alpha + ')';
}
function applyVgpColor(hex) {
  vgpEl.style.setProperty('--vgp-btn-bg', hexToRgba(hex, 0.16));
  vgpEl.style.setProperty('--vgp-btn-border', hexToRgba(hex, 0.5));
  vgpEl.style.setProperty('--vgp-btn-bg-pressed', hexToRgba(hex, 0.55));
  vgpEl.style.setProperty('--vgp-stick-bg', hexToRgba(hex, 0.12));
  vgpEl.style.setProperty('--vgp-stick-border', hexToRgba(hex, 0.4));
  vgpEl.style.setProperty('--vgp-knob-bg', hexToRgba(hex, 0.4));
  vgpEl.style.setProperty('--vgp-knob-border', hexToRgba(hex, 0.7));
}
vgpColorInput.value = settings.vgpColor || '#ffffff';
applyVgpColor(vgpColorInput.value);
vgpColorInput.oninput = function () {
  saveSettings({ vgpColor: vgpColorInput.value });
  applyVgpColor(vgpColorInput.value);
};

/* The face buttons are 4 fixed screen positions (top/left/right/bottom).
 * Which GAMEPAD_XB360_* index each position drives is constant across
 * every layout -- the layout picker is a pure skin (label + color), it
 * does not change behavior, so muscle memory built up on one skin (e.g.
 * always tapping "top" for the same in-game action) keeps working when
 * switching skins.
 *
 * These indices are NOT simply GAMEPAD_XB360_Y/X/B/A taken as-is, and
 * they are NOT the same compensation used for the real gamepad's mapping
 * above (that one compensates this specific physical controller's own
 * nonstandard button order as reported by the browser -- the virtual
 * overlay never goes through the browser's Gamepad API at all, so
 * there's nothing of that quirk to compensate for here). This exact
 * combination (Y/X natural, A/B swapped) is the one empirically
 * confirmed correct on-screen by the user testing each pair separately
 * -- Y/X needed the natural GAMEPAD_XB360_Y(17)/X(20) indices, while
 * A/B needed GAMEPAD_XB360_B(18)/A(19) swapped relative to their own
 * natural indices. If this ever breaks again (firmware/adapter change),
 * re-test each of the 4 face buttons individually -- don't assume the
 * two pairs behave the same way, they haven't so far. */
var FACE_SLOTS = { top: 17, left: 20, right: 18, bottom: 19 };
/* Skin only: label + color shown at each (fixed) position. Nintendo uses
 * the classic SNES-derived color set (X=blue, Y=green, A=red, B=yellow)
 * that Nintendo itself still uses in manuals/UI -- distinct on all four
 * buttons, unlike an earlier version of this table that accidentally
 * reused red for two of them. */
var FACE_SKIN_ALPHA = 0.55;
var FACE_SKINS = {
  xbox: {
    top: { label: 'Y', color: hexToRgba('#c99a00', FACE_SKIN_ALPHA) },
    left: { label: 'X', color: hexToRgba('#2f6fd0', FACE_SKIN_ALPHA) },
    right: { label: 'B', color: hexToRgba('#c73a3a', FACE_SKIN_ALPHA) },
    bottom: { label: 'A', color: hexToRgba('#3a9a4a', FACE_SKIN_ALPHA) }
  },
  playstation: {
    top: { label: '△', color: hexToRgba('#3aada3', FACE_SKIN_ALPHA) },
    left: { label: '□', color: hexToRgba('#c774b0', FACE_SKIN_ALPHA) },
    right: { label: '○', color: hexToRgba('#c75a5a', FACE_SKIN_ALPHA) },
    bottom: { label: '✕', color: hexToRgba('#4a7fd0', FACE_SKIN_ALPHA) }
  },
  nintendo: {
    top: { label: 'X', color: hexToRgba('#2f6fd0', FACE_SKIN_ALPHA) },
    left: { label: 'Y', color: hexToRgba('#2f9a5a', FACE_SKIN_ALPHA) },
    right: { label: 'A', color: hexToRgba('#c73a3a', FACE_SKIN_ALPHA) },
    bottom: { label: 'B', color: hexToRgba('#d0b400', FACE_SKIN_ALPHA) }
  }
};
/* Shoulder/trigger/menu/stick-click labels also follow each brand's own
 * real terms where one exists. Xbox has no official "L3/R3" branding
 * (that's Sony's) -- Microsoft's own docs call these the left/right
 * "thumbstick button", commonly shortened to LSB/RSB. Nintendo doesn't
 * brand them at all (Switch controller diagrams just show a stick-press
 * icon); LS/RS is the closest short, commonly understood label. */
var SHOULDER_LABELS = {
  xbox: { lb: 'LB', rb: 'RB', lt: 'LT', rt: 'RT', select: 'Back', start: 'Start', l3: 'LSB', r3: 'RSB' },
  playstation: { lb: 'L1', rb: 'R1', lt: 'L2', rt: 'R2', select: 'Share', start: 'Options', l3: 'L3', r3: 'R3' },
  nintendo: { lb: 'L', rb: 'R', lt: 'ZL', rt: 'ZR', select: '−', start: '+', l3: 'LS', r3: 'RS' }
};

var vgpFaceEls = {
  top: document.getElementById('vgp-face-top'),
  left: document.getElementById('vgp-face-left'),
  right: document.getElementById('vgp-face-right'),
  bottom: document.getElementById('vgp-face-bottom')
};
var vgpFaceContainer = document.getElementById('vgp-face');
var vgpLb = document.getElementById('vgp-lb');
var vgpRb = document.getElementById('vgp-rb');
var vgpLt = document.getElementById('vgp-lt');
var vgpRt = document.getElementById('vgp-rt');
var vgpSelectBtn = document.getElementById('vgp-select');
var vgpStartBtn = document.getElementById('vgp-start');
var vgpL3Btn = document.getElementById('vgp-l3');
var vgpR3Btn = document.getElementById('vgp-r3');
var vgpGuideBtn = document.getElementById('vgp-guide');

function applyVgpLayout(layout) {
  var skin = FACE_SKINS[layout] || FACE_SKINS.xbox;
  ['top', 'left', 'right', 'bottom'].forEach(function (pos) {
    var def = skin[pos];
    var el = vgpFaceEls[pos];
    el.textContent = def.label;
    el.style.background = def.color;
  });
  var labels = SHOULDER_LABELS[layout] || SHOULDER_LABELS.xbox;
  vgpLb.textContent = labels.lb;
  vgpRb.textContent = labels.rb;
  vgpLt.textContent = labels.lt;
  vgpRt.textContent = labels.rt;
  vgpSelectBtn.textContent = labels.select;
  vgpStartBtn.textContent = labels.start;
  vgpL3Btn.textContent = labels.l3;
  vgpR3Btn.textContent = labels.r3;
}

/* Symmetric (PlayStation-style: both sticks low, d-pad/face buttons
 * above them) vs. asymmetric (Xbox/Nintendo-style: left stick sits
 * ABOVE the d-pad instead). Only the left side ever swaps -- on real
 * pads of every brand the right side (face buttons above stick) is
 * already the same, so there is nothing to toggle there. Defaults to
 * whatever matches the chosen layout unless the user has explicitly
 * overridden it at least once (settings.stickSymmetric stays undefined
 * until then). */
function defaultSymmetric(layout) {
  return layout === 'playstation';
}
function applyStickSymmetry(symmetric) {
  vgpEl.classList.toggle('vgp-asym', !symmetric);
  vgpSymmetricBox.checked = symmetric;
}
vgpLayoutSelect.value = settings.vgpLayout || 'xbox';
applyVgpLayout(vgpLayoutSelect.value);
applyStickSymmetry(settings.stickSymmetric != null ? settings.stickSymmetric : defaultSymmetric(vgpLayoutSelect.value));
vgpLayoutSelect.onchange = function () {
  saveSettings({ vgpLayout: vgpLayoutSelect.value });
  applyVgpLayout(vgpLayoutSelect.value);
  if (settings.stickSymmetric == null) {
    applyStickSymmetry(defaultSymmetric(vgpLayoutSelect.value));
  }
};
vgpSymmetricBox.onchange = function () {
  saveSettings({ stickSymmetric: vgpSymmetricBox.checked });
  applyStickSymmetry(vgpSymmetricBox.checked);
};

/* --- free layout editing: "move buttons" lets the player drag any
 * control (or button cluster, for the d-pad/face group) to wherever
 * suits their hands/device, persisted in the same localStorage settings
 * blob as everything else on this page (so it survives reloads on this
 * browser, same mechanism as the volume/quality/etc. settings above). */
var vgpEditMode = false;
var vgpPositions = settings.vgpPositions || {};
var VGP_ANCHOR_IDS = [
  'vgp-lstick',
  'vgp-rstick',
  'vgp-dpad',
  'vgp-face',
  'vgp-l3',
  'vgp-r3',
  'vgp-lb',
  'vgp-lt',
  'vgp-rb',
  'vgp-rt',
  'vgp-select',
  'vgp-guide',
  'vgp-start'
];
function applyStoredPosition(el) {
  var pos = vgpPositions[el.id];
  if (!pos) return;
  el.style.left = pos.left;
  el.style.top = pos.top;
  el.style.right = 'auto';
  el.style.bottom = 'auto';
}
VGP_ANCHOR_IDS.forEach(function (id) {
  applyStoredPosition(document.getElementById(id));
});

/* Drags `el` as a whole (used directly for sticks/single buttons, and
 * with the shared `.vgp-face` container passed in for any of its 4
 * child buttons, so the diamond arrangement moves together). */
function startVgpDrag(el, e) {
  e.preventDefault();
  e.stopPropagation();
  el.setPointerCapture(e.pointerId);
  var rect = el.getBoundingClientRect();
  var offsetX = e.clientX - rect.left;
  var offsetY = e.clientY - rect.top;
  el.classList.add('dragging');
  function move(ev) {
    if (ev.pointerId !== e.pointerId) return;
    el.style.left = ev.clientX - offsetX + 'px';
    el.style.top = ev.clientY - offsetY + 'px';
    el.style.right = 'auto';
    el.style.bottom = 'auto';
  }
  function up(ev) {
    if (ev.pointerId !== e.pointerId) return;
    el.classList.remove('dragging');
    el.removeEventListener('pointermove', move);
    el.removeEventListener('pointerup', up);
    el.removeEventListener('pointercancel', up);
    vgpPositions[el.id] = { left: el.style.left, top: el.style.top };
    saveSettings({ vgpPositions: vgpPositions });
  }
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
}
vgpEditBox.checked = false;
vgpEditBox.onchange = function () {
  vgpEditMode = vgpEditBox.checked;
  vgpEl.classList.toggle('vgp-edit', vgpEditMode);
};
vgpResetBtn.onclick = function () {
  vgpPositions = {};
  saveSettings({ vgpPositions: {} });
  VGP_ANCHOR_IDS.forEach(function (id) {
    var el = document.getElementById(id);
    el.style.left = '';
    el.style.top = '';
    el.style.right = '';
    el.style.bottom = '';
  });
};

/* Generic press/release binding for a simple digital button. getIndices()
 * is called at press time (not bound once) so face buttons pick up
 * whichever slot the current layout assigns them. `dragEl` is the
 * element actually moved in edit mode -- the shared `.vgp-face`
 * container for the 4 face buttons, itself otherwise. */
function bindVgpButton(el, getIndices, dragEl) {
  el.addEventListener('pointerdown', function (e) {
    if (vgpEditMode) {
      startVgpDrag(dragEl || el, e);
      return;
    }
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    getIndices().forEach(function (i) {
      vgpTouchState[i] = 100;
    });
    /* Immediate feedback (zero-latency); the per-frame combiner below
     * confirms/maintains or clears this class every frame regardless, so
     * it also lights up correctly when a real gamepad presses it without
     * any touch involved. */
    el.classList.add('pressed');
  });
  function release() {
    getIndices().forEach(function (i) {
      vgpTouchState[i] = 0;
    });
  }
  el.addEventListener('pointerup', release);
  el.addEventListener('pointercancel', release);
}
bindVgpButton(vgpLb, function () {
  return [6];
});
bindVgpButton(vgpRb, function () {
  return [3];
});
bindVgpButton(vgpLt, function () {
  return [7];
});
bindVgpButton(vgpRt, function () {
  return [4];
});
bindVgpButton(vgpSelectBtn, function () {
  return [1];
});
bindVgpButton(vgpStartBtn, function () {
  return [2];
});
bindVgpButton(vgpGuideBtn, function () {
  return [0];
});

/* Declarative list used by the per-frame combiner below to toggle the
 * "pressed" class on every simple digital control (touch OR a real
 * gamepad, whichever is driving it) -- built once all the elements and
 * FACE_SLOTS exist. */
var VGP_DIGITAL_BINDINGS = [
  { el: vgpLb, idx: 6 },
  { el: vgpRb, idx: 3 },
  { el: vgpLt, idx: 7 },
  { el: vgpRt, idx: 4 },
  { el: vgpSelectBtn, idx: 1 },
  { el: vgpStartBtn, idx: 2 },
  { el: vgpGuideBtn, idx: 0 },
  { el: vgpL3Btn, idx: 8 },
  { el: vgpR3Btn, idx: 5 },
  { el: vgpFaceEls.top, idx: FACE_SLOTS.top },
  { el: vgpFaceEls.left, idx: FACE_SLOTS.left },
  { el: vgpFaceEls.right, idx: FACE_SLOTS.right },
  { el: vgpFaceEls.bottom, idx: FACE_SLOTS.bottom }
];
bindVgpButton(vgpL3Btn, function () {
  return [8];
});
bindVgpButton(vgpR3Btn, function () {
  return [5];
});
bindVgpButton(
  vgpFaceEls.top,
  function () {
    return [FACE_SLOTS.top];
  },
  vgpFaceContainer
);
bindVgpButton(
  vgpFaceEls.left,
  function () {
    return [FACE_SLOTS.left];
  },
  vgpFaceContainer
);
bindVgpButton(
  vgpFaceEls.right,
  function () {
    return [FACE_SLOTS.right];
  },
  vgpFaceContainer
);
bindVgpButton(
  vgpFaceEls.bottom,
  function () {
    return [FACE_SLOTS.bottom];
  },
  vgpFaceContainer
);

/* Draggable virtual stick: one finger captured per stick (a second finger
 * landing on an already-active stick is ignored), moved freely via
 * Pointer Events -- once captured, move/up events keep targeting this
 * element even if the finger slides outside its visible circle, so a
 * quick/wide swipe isn't lost. clientY grows downward, same sign
 * convention already used for the real gamepad's LY/RY axes (negative =
 * up), so no inversion is needed here.
 *
 * `key` ('l'/'r') records in vgpStickActive whether THIS stick currently
 * has a finger on it, so the per-frame combiner knows whether it's safe
 * to drive the knob's visual position from a real gamepad's axis instead
 * (never fight an actual finger for the knob's position; the two input
 * sources are still summed into the actual output regardless). */
var vgpStickActive = { l: false, r: false };
var vgpStickEls = {};
function bindVgpStick(el, ixX, ixY, key) {
  var knob = el.querySelector('.vgp-knob');
  vgpStickEls[key] = { el: el, knob: knob, ixX: ixX, ixY: ixY };
  var activePointer = null;
  function update(e) {
    var rect = el.getBoundingClientRect();
    var cx = rect.left + rect.width / 2;
    var cy = rect.top + rect.height / 2;
    var dx = e.clientX - cx;
    var dy = e.clientY - cy;
    var maxR = rect.width / 2;
    var dist = Math.sqrt(dx * dx + dy * dy);
    if (dist > maxR) {
      dx = (dx / dist) * maxR;
      dy = (dy / dist) * maxR;
    }
    knob.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
    vgpTouchState[ixX] = clamp100(dx / maxR);
    vgpTouchState[ixY] = clamp100(dy / maxR);
  }
  el.addEventListener('pointerdown', function (e) {
    if (vgpEditMode) {
      startVgpDrag(el, e);
      return;
    }
    e.preventDefault();
    if (activePointer !== null) return;
    activePointer = e.pointerId;
    vgpStickActive[key] = true;
    el.setPointerCapture(e.pointerId);
    update(e);
  });
  el.addEventListener('pointermove', function (e) {
    if (e.pointerId !== activePointer) return;
    update(e);
  });
  function release(e) {
    if (e.pointerId !== activePointer) return;
    activePointer = null;
    vgpStickActive[key] = false;
    vgpTouchState[ixX] = 0;
    vgpTouchState[ixY] = 0;
  }
  el.addEventListener('pointerup', release);
  el.addEventListener('pointercancel', release);
}
bindVgpStick(document.getElementById('vgp-lstick'), 11, 12, 'l'); // LX/LY
bindVgpStick(document.getElementById('vgp-rstick'), 9, 10, 'r'); // RX/RY

/* Single touch zone for the whole d-pad instead of 4 separate buttons:
 * up/down and left/right are each detected independently from the touch
 * position relative to the pad's center, so diagonals fall out for free
 * (e.g. up+right both end up active at once) instead of only ever the
 * single nearest direction. */
var vgpDpadArrows = null;
function bindVgpDpad(el) {
  var arrows = {
    up: el.querySelector('.vgp-dpad-up'),
    down: el.querySelector('.vgp-dpad-down'),
    left: el.querySelector('.vgp-dpad-left'),
    right: el.querySelector('.vgp-dpad-right')
  };
  vgpDpadArrows = arrows;
  var DEADZONE = 0.28;
  var activePointer = null;
  function setDir(index, on) {
    vgpTouchState[index] = on ? 100 : 0;
  }
  function update(e) {
    var rect = el.getBoundingClientRect();
    var dx = (e.clientX - (rect.left + rect.width / 2)) / (rect.width / 2);
    var dy = (e.clientY - (rect.top + rect.height / 2)) / (rect.height / 2);
    setDir(13, dy < -DEADZONE);
    setDir(14, dy > DEADZONE);
    setDir(15, dx < -DEADZONE);
    setDir(16, dx > DEADZONE);
  }
  function releaseAll() {
    setDir(13, false);
    setDir(14, false);
    setDir(15, false);
    setDir(16, false);
  }
  el.addEventListener('pointerdown', function (e) {
    if (vgpEditMode) {
      startVgpDrag(el, e);
      return;
    }
    e.preventDefault();
    if (activePointer !== null) return;
    activePointer = e.pointerId;
    el.setPointerCapture(e.pointerId);
    update(e);
  });
  el.addEventListener('pointermove', function (e) {
    if (e.pointerId !== activePointer) return;
    update(e);
  });
  function release(e) {
    if (e.pointerId !== activePointer) return;
    activePointer = null;
    releaseAll();
  }
  el.addEventListener('pointerup', release);
  el.addEventListener('pointercancel', release);
}
bindVgpDpad(document.getElementById('vgp-dpad'));

function setVirtualGamepadActive(active) {
  vgpEl.classList.toggle('hidden', !active);
  vgpLayoutRow.classList.toggle('hidden', !active);
  vgpSymmetricRow.classList.toggle('hidden', !active);
  vgpEditRow.classList.toggle('hidden', !active);
  vgpResetBtn.classList.toggle('hidden', !active);
  vgpColorRow.classList.toggle('hidden', !active);
  vgpOpacityRow.classList.toggle('hidden', !active);
  if (!active) {
    gamepadState.fill(0);
    vgpTouchState.fill(0);
    if (vgpEditMode) {
      vgpEditBox.checked = false;
      vgpEditMode = false;
      vgpEl.classList.remove('vgp-edit');
    }
  }
}

function setKeyboardModeActive(active) {
  if (!active) {
    gamepadState.fill(0);
    kbState.fill(0);
    kbHeldKeys = {};
    mouseStickX = 0;
    mouseStickY = 0;
    if (document.pointerLockElement === video) {
      document.exitPointerLock();
    }
  }
}

/* Refreshes the dropdown list from the gamepads actually detected by the
 * browser -- several can be connected at the same time, the user
 * explicitly picks which one to use rather than defaulting to the first
 * one found. */
function updateGamepadList() {
  /* A viewer never gets to choose a controller, so don't go looking for
   * one: the dropdown is hidden and the server would refuse whatever it
   * produced. Chrome also only reveals gamepad identities once they are
   * actually queried, so not querying is the quieter thing to do. */
  if (!playerControlsEnabled) {
    return;
  }
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  var connected = [];
  for (var i = 0; i < pads.length; i++) {
    if (pads[i]) connected.push(pads[i]);
  }

  gamepadSelect.innerHTML = '';
  var noneOpt = document.createElement('option');
  noneOpt.value = '';
  noneOpt.textContent = connected.length ? '(none chosen)' : 'none detected';
  gamepadSelect.appendChild(noneOpt);

  /* Always offered, whether or not the browser sees a real gamepad --
   * this is a touch overlay, not something Gamepad API detects. */
  var virtualOpt = document.createElement('option');
  virtualOpt.value = VIRTUAL_GAMEPAD_ID;
  virtualOpt.textContent = 'Virtual buttons (touch)';
  gamepadSelect.appendChild(virtualOpt);

  var keyboardOpt = document.createElement('option');
  keyboardOpt.value = KEYBOARD_GAMEPAD_ID;
  keyboardOpt.textContent = 'Keyboard/mouse';
  gamepadSelect.appendChild(keyboardOpt);

  var stillPresent = selectedGamepadId === VIRTUAL_GAMEPAD_ID || selectedGamepadId === KEYBOARD_GAMEPAD_ID;
  connected.forEach(function (gp) {
    var opt = document.createElement('option');
    opt.value = gp.id;
    opt.textContent = gp.id;
    if (gp.id === selectedGamepadId) {
      opt.selected = true;
      stillPresent = true;
    }
    gamepadSelect.appendChild(opt);
  });
  if (selectedGamepadId === VIRTUAL_GAMEPAD_ID || selectedGamepadId === KEYBOARD_GAMEPAD_ID) {
    gamepadSelect.value = selectedGamepadId;
  }

  /* If nothing is explicitly chosen and only one gamepad is plugged in,
   * pick it by default (the most common case) -- otherwise the user has
   * to choose themselves among several. */
  if (!selectedGamepadId && connected.length === 1) {
    selectedGamepadId = connected[0].id;
    gamepadSelect.value = selectedGamepadId;
  } else if (
    selectedGamepadId &&
    selectedGamepadId !== VIRTUAL_GAMEPAD_ID &&
    selectedGamepadId !== KEYBOARD_GAMEPAD_ID &&
    !stillPresent
  ) {
    currentGamepadStatus = 'disconnected (' + selectedGamepadId + ')';
  }
}
gamepadSelect.onchange = function () {
  selectedGamepadId = gamepadSelect.value || null;
  saveSettings({ gamepadId: selectedGamepadId });
  setVirtualGamepadActive(selectedGamepadId === VIRTUAL_GAMEPAD_ID);
  setKeyboardModeActive(selectedGamepadId === KEYBOARD_GAMEPAD_ID);
  updateRebindButtonVisibility();
  rebindEl.classList.add('hidden');
  rebindListening = null;
};
window.addEventListener('gamepadconnected', updateGamepadList);
window.addEventListener('gamepaddisconnected', updateGamepadList);
updateGamepadList();
setVirtualGamepadActive(selectedGamepadId === VIRTUAL_GAMEPAD_ID);
setKeyboardModeActive(selectedGamepadId === KEYBOARD_GAMEPAD_ID);
updateRebindButtonVisibility();

function getSelectedGamepad() {
  if (!selectedGamepadId) return null;
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    if (pads[i] && pads[i].id === selectedGamepadId) return pads[i];
  }
  return null;
}

/* Any real gamepad the browser currently sees, regardless of the
 * dropdown selection -- used only in "Virtual buttons" mode, where a
 * real controller (if plugged in) stays active alongside the touch
 * overlay rather than needing to be explicitly picked. Picks the first
 * one found; if more than one is connected while in virtual mode,
 * that's an edge case we don't try to disambiguate. */
function findAnyRealGamepad() {
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    if (pads[i]) return pads[i];
  }
  return null;
}

/* Connection time of each gamepad (by id), to ignore its very first
 * readings: many browsers only "discover" a gamepad the moment a button
 * is pressed on it, and that very first state (sometimes
 * unstable/stale for the first frames) used to be sent as-is -- observed
 * as a "phantom button" at the moment of detection. */
var gamepadConnectedAt = {};
window.addEventListener('gamepadconnected', function (e) {
  gamepadConnectedAt[e.gamepad.id] = performance.now();
});
var GAMEPAD_SETTLE_MS = 300;
