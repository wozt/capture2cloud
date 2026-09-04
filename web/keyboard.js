/*
 * capture2cloud -- keyboard and mouse as a controller
 *
 * Another entry in the same controller dropdown, so the same rebind
 * panel and the same merge apply. The mouse drives a stick, which is
 * why it needs pointer lock and a decay rather than raw deltas.
 */

/* --- "Keyboard/mouse" mode: another special entry in the same gamepad
 * dropdown. Keys drive the d-pad, left stick (as WASD-style digital
 * extremes) and every button; the mouse (via Pointer Lock, engaged by
 * clicking the video) drives the right stick continuously with a
 * self-centering decay. Both merge additively with a real gamepad too,
 * same philosophy as the virtual touch overlay above. Bindings are
 * saved as named profiles in localStorage (settings.keyboardProfiles),
 * switchable on the fly from the rebind panel. */
var KB_ROWS = [
  { field: 'guide', label: 'Guide' },
  { field: 'back', label: 'Back' },
  { field: 'start', label: 'Start' },
  { field: 'rb', label: 'RB' },
  { field: 'rt', label: 'RT' },
  { field: 'rs', label: 'RS (stick click)' },
  { field: 'lb', label: 'LB' },
  { field: 'lt', label: 'LT' },
  { field: 'ls', label: 'LS (stick click)' },
  { field: 'rx-', label: 'RX left', slot: 9, value: -100 },
  { field: 'rx+', label: 'RX right', slot: 9, value: 100 },
  { field: 'ry-', label: 'RY up', slot: 10, value: -100 },
  { field: 'ry+', label: 'RY down', slot: 10, value: 100 },
  { field: 'lx-', label: 'LX left', slot: 11, value: -100 },
  { field: 'lx+', label: 'LX right', slot: 11, value: 100 },
  { field: 'ly-', label: 'LY up', slot: 12, value: -100 },
  { field: 'ly+', label: 'LY down', slot: 12, value: 100 },
  { field: 'up', label: 'Up' },
  { field: 'down', label: 'Down' },
  { field: 'left', label: 'Left' },
  { field: 'right', label: 'Right' },
  { field: 'y', label: 'Y' },
  { field: 'b', label: 'B' },
  { field: 'a', label: 'A' },
  { field: 'x', label: 'X' }
];
/* Digital (non-axis) rows resolve to their GAMEPAD_XB360_* slot at
 * value 100 through this table; the 8 stick rows above carry their own
 * slot/value directly since two different keys can drive the same axis
 * in opposite directions. */
var KB_DIGITAL_SLOTS = {
  guide: 0, back: 1, start: 2, rb: 3, rt: 4, rs: 5, lb: 6, lt: 7, ls: 8,
  up: 13, down: 14, left: 15, right: 16, y: 17, b: 18, a: 19, x: 20
};
function kbRowSlotValue(row) {
  if (row.slot !== undefined) return { slot: row.slot, value: row.value };
  return { slot: KB_DIGITAL_SLOTS[row.field], value: 100 };
}
/* A profile binds each action to a keyboard key AND (optionally) a
 * mouse input -- either one triggers it, so e.g. RT can sit on both a
 * key and the right mouse button. Mouse ids are the tokens in
 * MOUSE_INPUTS below. */
function defaultKeyboardProfile() {
  return {
    keys: {
      up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight',
      'ly-': 'KeyW', 'ly+': 'KeyS', 'lx-': 'KeyA', 'lx+': 'KeyD',
      a: 'Space', b: 'ShiftLeft', x: 'ControlLeft', y: 'KeyF',
      lb: 'KeyQ', rb: 'KeyE', lt: 'Digit1', rt: 'Digit3',
      start: 'Enter', back: 'Backspace', guide: 'Tab',
      ls: 'KeyC', rs: 'KeyV'
    },
    /* Shooter-style defaults, the obvious mapping for a mouse: aim with
     * the right stick (movement, handled separately), shoot with the
     * right trigger, aim-down-sights with the left. */
    mouse: {
      rt: 'mouse0',
      lt: 'mouse2'
    }
  };
}

/* Everything a mouse can be bound to, besides movement (which always
 * drives the right stick). Wheel notches are momentary: they have no
 * "release" event, so they are held for MOUSE_WHEEL_PULSE_MS. */
var MOUSE_INPUTS = [
  { id: '', label: '(none)' },
  { id: 'mouse0', label: 'Left click' },
  { id: 'mouse1', label: 'Middle click' },
  { id: 'mouse2', label: 'Right click' },
  { id: 'mouse3', label: 'Mouse 4 (back)' },
  { id: 'mouse4', label: 'Mouse 5 (forward)' },
  { id: 'wheelup', label: 'Wheel up' },
  { id: 'wheeldown', label: 'Wheel down' }
];
var MOUSE_WHEEL_PULSE_MS = 80;
function getKeyboardProfiles() {
  var profiles = settings.keyboardProfiles || { Default: defaultKeyboardProfile() };
  /* Be tolerant of a profile that predates the {keys,mouse} shape even
   * if migration didn't see it (hand-edited storage, say). */
  for (var name in profiles) {
    if (profiles[name] && !profiles[name].keys) {
      profiles[name] = { keys: profiles[name], mouse: {} };
    }
    if (!profiles[name].mouse) profiles[name].mouse = {};
  }
  return profiles;
}
function getActiveKeyboardProfileName() {
  var profiles = getKeyboardProfiles();
  var name = settings.activeKeyboardProfile;
  return name && profiles[name] ? name : Object.keys(profiles)[0];
}
function getActiveKeyboardProfile() {
  return getKeyboardProfiles()[getActiveKeyboardProfileName()];
}
function saveKeyboardProfiles(profiles, activeName) {
  var patch = { keyboardProfiles: profiles };
  if (activeName) patch.activeKeyboardProfile = activeName;
  saveSettings(patch);
}

var kbCodeToRow = {};   /* keyboard code  -> row */
var kbMouseToRow = {};  /* mouse input id -> row */
function rebuildKbCodeToRow() {
  kbCodeToRow = {};
  kbMouseToRow = {};
  var profile = getActiveKeyboardProfile();
  KB_ROWS.forEach(function (row) {
    var code = profile.keys[row.field];
    if (code) kbCodeToRow[code] = row;
    var mouse = profile.mouse[row.field];
    if (mouse) kbMouseToRow[mouse] = row;
  });
}
/* Mouse inputs currently held, same idea as kbHeldKeys. Wheel entries
 * are timestamps: they expire on their own (see recomputeKbState). */
var kbHeldMouse = {};

function recomputeKbState() {
  kbState.fill(0);
  var apply = function (row) {
    if (!row) return;
    var sv = kbRowSlotValue(row);
    kbState[sv.slot] = Math.max(-100, Math.min(100, kbState[sv.slot] + sv.value));
  };
  for (var code in kbHeldKeys) {
    if (kbHeldKeys[code]) apply(kbCodeToRow[code]);
  }
  var now = performance.now();
  for (var id in kbHeldMouse) {
    var held = kbHeldMouse[id];
    if (held === true) {
      apply(kbMouseToRow[id]);
    } else if (typeof held === 'number') {
      /* A wheel pulse: still counts until it times out. */
      if (now - held < MOUSE_WHEEL_PULSE_MS) {
        apply(kbMouseToRow[id]);
      } else {
        kbHeldMouse[id] = false;
      }
    }
  }
}
rebuildKbCodeToRow();

/* Mouse -> right stick (RX/RY), via Pointer Lock: click the video while
 * in keyboard mode to engage, Escape (browser default) or clicking
 * elsewhere releases it. Deltas accumulate then decay back to center
 * each frame when the mouse stops moving, for a rough self-centering
 * "flick" feel rather than an absolute position. */
var MOUSE_SENSITIVITY = 0.6;
var MOUSE_DECAY = 0.85;
function updateMouseStick() {
  mouseStickX *= MOUSE_DECAY;
  mouseStickY *= MOUSE_DECAY;
  if (Math.abs(mouseStickX) < 0.5) mouseStickX = 0;
  if (Math.abs(mouseStickY) < 0.5) mouseStickY = 0;
}
document.addEventListener('mousemove', function (e) {
  if (document.pointerLockElement !== video) return;
  mouseStickX = Math.max(-100, Math.min(100, mouseStickX + e.movementX * MOUSE_SENSITIVITY));
  mouseStickY = Math.max(-100, Math.min(100, mouseStickY + e.movementY * MOUSE_SENSITIVITY));
});
video.addEventListener('click', function () {
  if (selectedGamepadId === KEYBOARD_GAMEPAD_ID && document.pointerLockElement !== video) {
    video.requestPointerLock();
  }
});

/* True while the mouse is genuinely "in the game": the pointer is
 * locked to the video. Outside that, the cursor is a normal cursor and
 * clicks belong to whatever they land on. */
function mouseIsPlaying() {
  return document.pointerLockElement === video;
}

/* True if an event landed on the page's own UI (settings bar, rebind
 * panel) rather than on the stream. Input there must never reach the
 * console: pressing a key to type in a field, or clicking a button,
 * would otherwise also fire whatever that input is bound to. */
function isUiTarget(e) {
  var el = e.target;
  while (el) {
    if (el === bar || el === rebindEl) return true;
    el = el.parentElement;
  }
  return false;
}

/* Mouse buttons and wheel as bindable inputs. Bound to the whole
 * document rather than the video element: once pointer-locked the
 * events still target the document, and a click that started on the
 * video can end anywhere. */
function mouseInputActive(id, active) {
  if (!kbMouseToRow[id]) return false; /* not bound to anything */
  kbHeldMouse[id] = active;
  recomputeKbState();
  return true;
}

document.addEventListener('mousedown', function (e) {
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  /* Only while actually playing: with the pointer unlocked the cursor
   * is back and the menu has to stay clickable. preventDefault() here
   * on a menu click would swallow it entirely -- which is exactly what
   * made the settings bar unusable in this mode. */
  if (!mouseIsPlaying() || isUiTarget(e)) return;
  if (mouseInputActive('mouse' + e.button, true)) {
    e.preventDefault();
  }
});
/* Releases are always processed, even over the UI or after the pointer
 * was unlocked mid-click: skipping one would leave the button stuck
 * down on the console. */
document.addEventListener('mouseup', function (e) {
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  mouseInputActive('mouse' + e.button, false);
});
/* Any exit from pointer lock (Escape, alt-tab, clicking away) means the
 * player has stopped playing: drop every held mouse input rather than
 * leaving it pressed on the console. */
document.addEventListener('pointerlockchange', function () {
  if (!mouseIsPlaying()) {
    kbHeldMouse = {};
    recomputeKbState();
  }
});
/* Right-click must not open the context menu while playing. */
document.addEventListener('contextmenu', function (e) {
  if (selectedGamepadId === KEYBOARD_GAMEPAD_ID && document.pointerLockElement === video) {
    e.preventDefault();
  }
});
document.addEventListener('wheel', function (e) {
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  if (!mouseIsPlaying() || isUiTarget(e)) return; /* let the menu scroll */
  var id = e.deltaY < 0 ? 'wheelup' : 'wheeldown';
  if (!kbMouseToRow[id]) return;
  e.preventDefault();
  /* A wheel notch has no release event, so record when it happened and
   * let recomputeKbState() expire it after MOUSE_WHEEL_PULSE_MS. */
  kbHeldMouse[id] = performance.now();
  recomputeKbState();
}, { passive: false });
