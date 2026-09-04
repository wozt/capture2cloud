/*
 * capture2cloud -- the two panels that open over everything
 *
 * The rebind panel, shared by real controllers and by the keyboard,
 * and the controller test that draws a pad and lights up whatever is
 * pressed.
 *
 * Loaded before touchpad.js because the touch pad asks, as it starts,
 * whether the rebind button should be visible -- and that question is
 * answered here.
 */

/* --- rebind panel: shared UI for "real gamepad" (button/axis capture)
 * and "keyboard/mouse" (key capture) modes. Opening it puts the chosen
 * field into a "listening" state; the next matching input (a new
 * button press / axis motion for gamepad, or any keydown for keyboard)
 * captures and saves it, then the panel re-renders. (DOM refs and the
 * rebindListening/rebindGpSnapshot state live near the top of the file,
 * alongside the other early state -- see the note there.) */
function updateRebindButtonVisibility() {
  var isRealGamepad =
    selectedGamepadId && selectedGamepadId !== VIRTUAL_GAMEPAD_ID && selectedGamepadId !== KEYBOARD_GAMEPAD_ID;
  rebindBtn.classList.toggle('hidden', !(isRealGamepad || selectedGamepadId === KEYBOARD_GAMEPAD_ID));
}

function gamepadInputLabel(f) {
  if (!f) return '(unbound)';
  if (f.type === 'axis') return 'axis ' + f.index;
  return 'button ' + f.index;
}

function renderRebindPanel() {
  rebindRowsEl.innerHTML = '';
  rebindProfileRow.innerHTML = '';
  var isKeyboard = selectedGamepadId === KEYBOARD_GAMEPAD_ID;

  if (isKeyboard) {
    rebindTitle.textContent = 'Rebind keyboard/mouse';
    rebindNote.textContent =
      'Each action can have a key AND a mouse input -- either one triggers it. ' +
      'Mouse movement always drives the right stick while pointer-locked (click the video to engage, Escape to release); ' +
      'the RX/RY rows are an optional key-based alternative to that.';

    var profiles = getKeyboardProfiles();
    var activeName = getActiveKeyboardProfileName();
    var select = document.createElement('select');
    Object.keys(profiles).forEach(function (name) {
      var opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      if (name === activeName) opt.selected = true;
      select.appendChild(opt);
    });
    select.onchange = function () {
      saveKeyboardProfiles(profiles, select.value);
      rebuildKbCodeToRow();
      renderRebindPanel();
    };
    var saveAsBtn = document.createElement('button');
    saveAsBtn.textContent = 'Save as...';
    saveAsBtn.onclick = function () {
      var name = prompt('New profile name:', activeName + ' copy');
      if (!name) return;
      profiles[name] = JSON.parse(JSON.stringify(profiles[activeName]));
      saveKeyboardProfiles(profiles, name);
      rebuildKbCodeToRow();
      renderRebindPanel();
    };
    var resetBtn = document.createElement('button');
    resetBtn.textContent = 'Reset to default';
    resetBtn.onclick = function () {
      profiles[activeName] = defaultKeyboardProfile();
      saveKeyboardProfiles(profiles, activeName);
      rebuildKbCodeToRow();
      renderRebindPanel();
    };
    rebindProfileRow.appendChild(select);
    rebindProfileRow.appendChild(saveAsBtn);
    rebindProfileRow.appendChild(resetBtn);

    var activeProfile = profiles[activeName];
    KB_ROWS.forEach(function (row) {
      var rowEl = document.createElement('div');
      rowEl.className = 'rebind-row';

      var nameEl = document.createElement('span');
      nameEl.className = 'rb-name';
      nameEl.textContent = row.label;

      /* --- keyboard half: shows the bound key, captured by listening --- */
      var valueEl = document.createElement('span');
      valueEl.className = 'rb-value';
      valueEl.textContent = activeProfile.keys[row.field] || '(no key)';
      var btn = document.createElement('button');
      var listening = rebindListening && rebindListening.kind === 'keyboard' && rebindListening.field === row.field;
      btn.textContent = listening ? 'press a key...' : 'key';
      if (listening) btn.classList.add('listening');
      btn.onclick = function () {
        rebindListening = { kind: 'keyboard', field: row.field };
        renderRebindPanel();
      };

      /* --- mouse half: a plain dropdown rather than a capture. Mouse
       * buttons are a short, fixed list, so picking one is quicker and
       * less error-prone than clicking to record it -- and it avoids the
       * awkwardness of "click to bind a click". --- */
      var mouseSel = document.createElement('select');
      mouseSel.className = 'rb-mouse';
      MOUSE_INPUTS.forEach(function (mi) {
        var opt = document.createElement('option');
        opt.value = mi.id;
        opt.textContent = mi.label;
        if ((activeProfile.mouse[row.field] || '') === mi.id) opt.selected = true;
        mouseSel.appendChild(opt);
      });
      mouseSel.onchange = function () {
        applyMouseBinding(row.field, mouseSel.value);
      };

      rowEl.appendChild(nameEl);
      rowEl.appendChild(valueEl);
      rowEl.appendChild(btn);
      rowEl.appendChild(mouseSel);
      rebindRowsEl.appendChild(rowEl);
    });
  } else {
    var gp = getSelectedGamepad();
    rebindTitle.textContent = 'Rebind gamepad' + (gp ? ' -- ' + gp.id : '');
    rebindNote.textContent = 'Click "listen" then press the button (or move the axis) on the real gamepad you want bound to that row.';
    if (!gp) {
      rebindNote.textContent = 'No gamepad currently selected/connected.';
      return;
    }
    var gpProfile = getGamepadProfile(gp.id);
    var resetGpBtn = document.createElement('button');
    resetGpBtn.textContent = 'Reset to default';
    resetGpBtn.onclick = function () {
      saveGamepadProfile(gp.id, defaultGamepadProfile(gp.id));
      renderRebindPanel();
    };
    rebindProfileRow.appendChild(resetGpBtn);

    GAMEPAD_ROWS.forEach(function (row) {
      var rowEl = document.createElement('div');
      rowEl.className = 'rebind-row';
      var nameEl = document.createElement('span');
      nameEl.className = 'rb-name';
      nameEl.textContent = row.label;
      var valueEl = document.createElement('span');
      valueEl.className = 'rb-value';
      valueEl.textContent = gamepadInputLabel(gpProfile[row.field]);
      var btn = document.createElement('button');
      var listening = rebindListening && rebindListening.kind === 'gamepad' && rebindListening.field === row.field;
      btn.textContent = listening ? 'press/move...' : 'listen';
      if (listening) btn.classList.add('listening');
      btn.onclick = function () {
        rebindListening = { kind: 'gamepad', field: row.field };
        rebindGpSnapshot = null;
        renderRebindPanel();
      };
      rowEl.appendChild(nameEl);
      rowEl.appendChild(valueEl);
      rowEl.appendChild(btn);
      rebindRowsEl.appendChild(rowEl);
    });
  }
}

rebindBtn.onclick = function () {
  rebindEl.classList.remove('hidden');
  renderRebindPanel();
};
rebindCloseBtn.onclick = function () {
  rebindEl.classList.add('hidden');
  rebindListening = null;
  rebindGpSnapshot = null;
};

function applyGamepadBinding(field, entry) {
  var gp = getSelectedGamepad();
  if (!gp) return;
  /* Copied before being changed: getGamepadProfile() may hand back the
   * shared cached defaults, and writing through that would quietly turn
   * "the defaults for this pad" into "whatever was last rebound". */
  var from = getGamepadProfile(gp.id);
  var profile = {};
  for (var k in from) profile[k] = from[k];
  profile[field] = entry;
  saveGamepadProfile(gp.id, profile);
  rebindListening = null;
  rebindGpSnapshot = null;
  renderRebindPanel();
}
function applyKeyboardBinding(field, code) {
  var profiles = getKeyboardProfiles();
  var activeName = getActiveKeyboardProfileName();
  profiles[activeName].keys[field] = code;
  saveKeyboardProfiles(profiles, activeName);
  rebuildKbCodeToRow();
  rebindListening = null;
  renderRebindPanel();
}

function applyMouseBinding(field, mouseId) {
  var profiles = getKeyboardProfiles();
  var activeName = getActiveKeyboardProfileName();
  if (mouseId) {
    profiles[activeName].mouse[field] = mouseId;
  } else {
    delete profiles[activeName].mouse[field];
  }
  saveKeyboardProfiles(profiles, activeName);
  rebuildKbCodeToRow();
  /* Any held state for the old binding would otherwise stay stuck. */
  kbHeldMouse = {};
  recomputeKbState();
  rebindListening = null;
  renderRebindPanel();
}

/* Polls the currently-selected real gamepad for a NEW button press or a
 * significant axis move (compared to a snapshot taken when listening
 * started), to capture it as the binding for rebindListening.field.
 * Called every frame from gamepadLoop() below; near-zero cost when not
 * actively rebinding. */
function pollGamepadRebind() {
  if (!rebindListening || rebindListening.kind !== 'gamepad') return;
  var gp = getSelectedGamepad();
  if (!gp) return;
  if (!rebindGpSnapshot) {
    rebindGpSnapshot = {
      buttons: gp.buttons.map(function (btn) {
        return btn.pressed;
      }),
      axes: gp.axes.slice()
    };
    return;
  }
  for (var i = 0; i < gp.buttons.length; i++) {
    if (gp.buttons[i].pressed && !rebindGpSnapshot.buttons[i]) {
      applyGamepadBinding(rebindListening.field, { type: 'button', index: i });
      return;
    }
  }
  for (var j = 0; j < gp.axes.length; j++) {
    if (Math.abs(gp.axes[j] - (rebindGpSnapshot.axes[j] || 0)) > 0.5) {
      applyGamepadBinding(rebindListening.field, { type: 'axis', index: j });
      return;
    }
  }
}

document.addEventListener('keydown', function (e) {
  if (rebindListening && rebindListening.kind === 'keyboard') {
    e.preventDefault();
    applyKeyboardBinding(rebindListening.field, e.code);
    return;
  }
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  /* Typing into the settings bar (a password prompt, a profile name)
   * or the rebind panel must not also press whatever that key is bound
   * to on the console. Only key PRESSES are filtered this way; releases
   * below are always processed, so a key held while playing and let go
   * over the UI can't stay stuck down. */
  if (isUiTarget(e)) return;
  if (kbCodeToRow[e.code]) e.preventDefault();
  if (kbHeldKeys[e.code]) return; // ignore key-repeat
  kbHeldKeys[e.code] = true;
  recomputeKbState();
});
document.addEventListener('keyup', function (e) {
  if (kbHeldKeys[e.code]) {
    kbHeldKeys[e.code] = false;
    recomputeKbState();
  }
});

/* Combines keyboard + mouse + a real gamepad (if any) the same way
 * combineVirtualGamepadState() combines touch + a real gamepad --
 * additive, saturating. Doesn't drive any on-screen visuals (there is
 * no overlay in this mode). */

/* ===================== controller test popup =========================
 *
 * Draws a controller and lights up whatever is currently pressed, in
 * one of three familiar shapes. Three deliberate choices:
 *
 * - It reflects `gamepadState`, i.e. the 21 values actually being SENT
 *   to the console, not the raw browser Gamepad API. So it tests the
 *   whole chain -- device, mapping, rebinds, virtual pad, keyboard --
 *   rather than just whether the browser sees a controller. If a
 *   button lights up here, that is what the console receives.
 *
 * - The layouts differ in more than labels. Nintendo's face buttons are
 *   a diagonal mirror of Xbox's (X on top, B at the bottom, A on the
 *   right), so the same physical press lands on a different position;
 *   PlayStation keeps Xbox's positions with its own glyphs.
 *
 * - Input is blocked by default while the popup is open (a checkbox
 *   lets it through): pressing every button to check them shouldn't
 *   also press them in the running game. */

var padTestEl = document.getElementById('padtest');
var padTestBtn = document.getElementById('padtest-btn');
var padTestClose = document.getElementById('padtest-close');
var padTestLayout = document.getElementById('padtest-layout');
var padTestBlock = document.getElementById('padtest-block');
var padTestSvg = document.getElementById('padtest-svg');
var padTestReadout = document.getElementById('padtest-readout');
var padTestSource = document.getElementById('padtest-source');

/* Slots, from gamepad_bridge.h's GAMEPAD_XB360_* order. */
var PT = { GUIDE: 0, BACK: 1, START: 2, RB: 3, RT: 4, RS: 5, LB: 6, LT: 7, LS: 8,
           RX: 9, RY: 10, LX: 11, LY: 12, UP: 13, DOWN: 14, LEFT: 15, RIGHT: 16,
           Y: 17, B: 18, A: 19, X: 20 };

/* A controller silhouette: central body plus two grips. Shared by every
 * layout -- only the controls on top of it move. */
/* Traced clockwise from the top centre. The notch between the grips has
 * to stay LOW (y~182): a shallower one cuts straight through where the
 * d-pad and the right stick sit, leaving them floating outside the
 * silhouette. */
var PAD_BODY =
  'M 200,58 C 252,58 288,50 324,50 C 364,50 390,84 388,134 ' +
  'C 386,186 370,224 338,224 C 312,224 296,206 288,188 ' +
  'C 280,184 250,182 200,182 C 150,182 120,184 112,188 ' +
  'C 104,206 88,224 62,224 C 30,224 14,186 12,134 ' +
  'C 10,84 36,50 76,50 C 112,50 148,58 200,58 Z';

var PAD_LAYOUTS = {
  xbox: {
    name: 'Xbox',
    lstick: { x: 104, y: 100 }, rstick: { x: 252, y: 156 },
    dpad: { x: 152, y: 156 }, faceCx: 300, faceCy: 104,
    face: { top: { slot: PT.Y, label: 'Y', color: '#c99a00' },
            left: { slot: PT.X, label: 'X', color: '#2f6fd0' },
            right: { slot: PT.B, label: 'B', color: '#c73a3a' },
            bottom: { slot: PT.A, label: 'A', color: '#3a9a4a' } },
    small: [ { slot: PT.BACK, x: 176, y: 106, label: '\u29c9' },
             { slot: PT.START, x: 224, y: 106, label: '\u2261' },
             { slot: PT.GUIDE, x: 200, y: 80, label: '\u2302', r: 12 } ],
    shoulder: { lb: 'LB', rb: 'RB', lt: 'LT', rt: 'RT' }
  },
  switch: {
    name: 'Switch Pro',
    lstick: { x: 104, y: 100 }, rstick: { x: 252, y: 156 },
    dpad: { x: 152, y: 156 }, faceCx: 300, faceCy: 104,
    /* Diagonal mirror of Xbox: X up, B down, A right, Y left. */
    face: { top: { slot: PT.X, label: 'X', color: '#2f6fd0' },
            left: { slot: PT.Y, label: 'Y', color: '#2f9a5a' },
            right: { slot: PT.A, label: 'A', color: '#c73a3a' },
            bottom: { slot: PT.B, label: 'B', color: '#d0b400' } },
    small: [ { slot: PT.BACK, x: 176, y: 106, label: '\u2212' },
             { slot: PT.START, x: 224, y: 106, label: '+' },
             { slot: PT.GUIDE, x: 200, y: 80, label: '\u2302', r: 12 } ],
    shoulder: { lb: 'L', rb: 'R', lt: 'ZL', rt: 'ZR' }
  },
  ps5: {
    name: 'DualSense',
    /* Symmetric sticks, d-pad up on the left. */
    lstick: { x: 158, y: 158 }, rstick: { x: 246, y: 158 },
    dpad: { x: 104, y: 100 }, faceCx: 300, faceCy: 104,
    face: { top: { slot: PT.Y, label: '\u25b3', color: '#3aada3' },
            left: { slot: PT.X, label: '\u25a1', color: '#c774b0' },
            right: { slot: PT.B, label: '\u25cb', color: '#c75a5a' },
            bottom: { slot: PT.A, label: '\u2715', color: '#4a7fd0' } },
    small: [ { slot: PT.BACK, x: 168, y: 96, label: '\u29c9' },
             { slot: PT.START, x: 236, y: 96, label: '\u2261' },
             { slot: PT.GUIDE, x: 202, y: 128, label: 'PS', r: 11 } ],
    shoulder: { lb: 'L1', rb: 'R1', lt: 'L2', rt: 'R2' }
  }
};

var padTestNodes = null; /* built per layout, then only updated */

function svgEl(tag, attrs) {
  var el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (var k in attrs) el.setAttribute(k, attrs[k]);
  return el;
}

function buildPadTest(layoutName) {
  var L = PAD_LAYOUTS[layoutName] || PAD_LAYOUTS.xbox;
  padTestSvg.innerHTML = '';
  var n = { buttons: [], sticks: {}, triggers: {} };

  padTestSvg.appendChild(svgEl('path', { d: PAD_BODY, class: 'pt-body' }));

  /* A round or rect button plus its label, remembered for updates. */
  function addButton(slot, x, y, r, label, color, shape, w, h) {
    var el;
    if (shape === 'rect') {
      el = svgEl('rect', { x: x - w / 2, y: y - h / 2, width: w, height: h, rx: h / 2,
                           class: 'pt-btn', fill: color || '#ffffff' });
    } else {
      el = svgEl('circle', { cx: x, cy: y, r: r, class: 'pt-btn', fill: color || '#ffffff' });
    }
    padTestSvg.appendChild(el);
    var t = svgEl('text', { x: x, y: y, class: 'pt-label' });
    t.textContent = label;
    padTestSvg.appendChild(t);
    n.buttons.push({ slot: slot, el: el, label: t });
  }

  /* Sticks: a well with a knob that follows the axes, and lights up on
   * click (L3/R3). */
  function addStick(key, cfg, xSlot, ySlot, clickSlot, name) {
    padTestSvg.appendChild(svgEl('circle', { cx: cfg.x, cy: cfg.y, r: 24, class: 'pt-well' }));
    var knob = svgEl('circle', { cx: cfg.x, cy: cfg.y, r: 13, class: 'pt-knob' });
    padTestSvg.appendChild(knob);
    var t = svgEl('text', { x: cfg.x, y: cfg.y + 36, class: 'pt-label' });
    t.textContent = name;
    padTestSvg.appendChild(t);
    n.sticks[key] = { knob: knob, cx: cfg.x, cy: cfg.y, xSlot: xSlot, ySlot: ySlot, clickSlot: clickSlot };
  }

  /* Triggers are analog: the fill height tracks the value. */
  function addTrigger(key, slot, x, label) {
    var top = 10, h = 28, w = 36;
    padTestSvg.appendChild(svgEl('rect', { x: x - w / 2, y: top, width: w, height: h, rx: 6, class: 'pt-well' }));
    var fill = svgEl('rect', { x: x - w / 2, y: top + h, width: w, height: 0, rx: 6, class: 'pt-trigger-fill' });
    padTestSvg.appendChild(fill);
    var t = svgEl('text', { x: x, y: top + h / 2, class: 'pt-label' });
    t.textContent = label;
    padTestSvg.appendChild(t);
    n.triggers[key] = { fill: fill, slot: slot, top: top, h: h };
  }

  /* Triggers sit above the shoulders, which in turn straddle the body's
   * top edge -- the schematic "seen from above and behind" arrangement. */
  addTrigger('lt', PT.LT, 80, L.shoulder.lt);
  addTrigger('rt', PT.RT, 320, L.shoulder.rt);
  addButton(PT.LB, 80, 48, 0, L.shoulder.lb, '#ffffff', 'rect', 48, 16);
  addButton(PT.RB, 320, 48, 0, L.shoulder.rb, '#ffffff', 'rect', 48, 16);

  /* D-pad: four arms around its centre. */
  var d = L.dpad, arm = 15, thick = 13;
  addButton(PT.UP, d.x, d.y - arm, 0, '▲', '#ffffff', 'rect', thick, 20);
  addButton(PT.DOWN, d.x, d.y + arm, 0, '▼', '#ffffff', 'rect', thick, 20);
  addButton(PT.LEFT, d.x - arm, d.y, 0, '◀', '#ffffff', 'rect', 20, thick);
  addButton(PT.RIGHT, d.x + arm, d.y, 0, '▶', '#ffffff', 'rect', 20, thick);

  var fr = 26;
  addButton(L.face.top.slot, L.faceCx, L.faceCy - fr, 13, L.face.top.label, L.face.top.color);
  addButton(L.face.bottom.slot, L.faceCx, L.faceCy + fr, 13, L.face.bottom.label, L.face.bottom.color);
  addButton(L.face.left.slot, L.faceCx - fr, L.faceCy, 13, L.face.left.label, L.face.left.color);
  addButton(L.face.right.slot, L.faceCx + fr, L.faceCy, 13, L.face.right.label, L.face.right.color);

  L.small.forEach(function (s) {
    addButton(s.slot, s.x, s.y, s.r || 9, s.label, '#ffffff');
  });

  addStick('l', L.lstick, PT.LX, PT.LY, PT.LS, 'L3');
  addStick('r', L.rstick, PT.RX, PT.RY, PT.RS, 'R3');

  padTestNodes = n;
}

/* Called every frame while the popup is open. */
function updatePadTest() {
  if (!padTestNodes) return;
  var s = gamepadState;

  padTestNodes.buttons.forEach(function (b) {
    b.el.classList.toggle('on', s[b.slot] !== 0);
  });

  for (var k in padTestNodes.sticks) {
    var st = padTestNodes.sticks[k];
    /* 24 is the well radius, 13 the knob's: keep the knob inside. */
    st.knob.setAttribute('cx', st.cx + (s[st.xSlot] / 100) * 11);
    st.knob.setAttribute('cy', st.cy + (s[st.ySlot] / 100) * 11);
    st.knob.classList.toggle('on', s[st.clickSlot] !== 0);
  }

  for (var t in padTestNodes.triggers) {
    var tr = padTestNodes.triggers[t];
    var frac = Math.min(1, Math.abs(s[tr.slot]) / 100);
    tr.fill.setAttribute('y', tr.top + tr.h * (1 - frac));
    tr.fill.setAttribute('height', tr.h * frac);
  }

  /* Raw values, so an axis that drifts or a button stuck at a partial
   * value is visible rather than merely "not lighting up". */
  var active = [];
  for (var i = 0; i < s.length; i++) {
    if (s[i] !== 0) active.push(PT_NAMES[i] + '=' + s[i]);
  }
  padTestReadout.textContent = active.length ? active.join('  ') : '(nothing pressed)';
  /* Composed fresh each frame. Appending the suffix to
   * currentGamepadStatus instead would re-append it on every one of the
   * 60 frames per second, growing the line without bound. */
  padTestSource.textContent = 'source: ' + currentGamepadStatus +
    (inputIsSuppressed() ? ' -- input blocked while testing' : '');
}

var PT_NAMES = ['GUIDE','BACK','START','RB','RT','RS','LB','LT','LS','RX','RY','LX','LY',
                'UP','DOWN','LEFT','RIGHT','Y','B','A','X'];

function padTestIsOpen() {
  return !padTestEl.classList.contains('hidden');
}

padTestBtn.onclick = function () {
  padTestEl.classList.remove('hidden');
  buildPadTest(padTestLayout.value);
};
padTestClose.onclick = function () {
  padTestEl.classList.add('hidden');
};
padTestLayout.value = settings.padTestLayout || 'xbox';
padTestLayout.onchange = function () {
  saveSettings({ padTestLayout: padTestLayout.value });
  buildPadTest(padTestLayout.value);
};
