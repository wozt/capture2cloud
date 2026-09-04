/*
 * capture2cloud -- the page's own preamble
 *
 * These are the things every other file needs to exist before it runs,
 * which is why this one is loaded first: the elements, looked up once
 * rather than by every handler that touches them, and the error hook
 * that puts a stack-less "JS error" line in the status row instead of
 * losing it to a console nobody has open.
 *
 * The files after this one are plain scripts sharing one scope, not
 * modules. That is deliberate: the page is one program built around a
 * shared set of elements and a shared settings object, and rewriting
 * 253 top-level names into imports and exports would be a larger,
 * riskier change than the one being asked for -- and would break every
 * test that reaches into those names to check them.
 */

/* Which release this is. Shown in the control bar so "which version am
 * I on" is answerable by looking, and pinned against version.h and the
 * Android build by a test -- three files giving three answers is worse
 * than none of them giving any. */
var C2C_VERSION = '1.2.4';

window.onerror = function (msg, url, line, col, err) {
  var st = document.getElementById('st');
  if (st) st.textContent = 'JS error: ' + msg + ' @' + line;
};

var statusEl = document.getElementById('st');
var infoEl = document.getElementById('info');
var video = document.getElementById('v');
var canvas = document.getElementById('c');
var gate = document.getElementById('gate');
var gateBtn = document.getElementById('gate-btn');
var muteBtn = document.getElementById('mute');
/* The collapsible groups in the control bar. Held as a list because two
 * things are done to all of them: only one opens at a time, and a viewer
 * -- who is allowed none of what is inside -- gets none of them at all,
 * rather than five headings that open onto nothing. */
var menuGroups = ['group-stream', 'group-picture', 'group-input', 'group-touch', 'group-console']
  .map(function (id) { return document.getElementById(id); })
  .filter(Boolean);

menuGroups.forEach(function (group) {
  group.addEventListener('toggle', function () {
    if (!group.open) return;
    menuGroups.forEach(function (other) {
      if (other !== group) other.open = false;
    });
  });
});

var wakeConsoleBtn = document.getElementById('wake-console');
var resetDongleBtn = document.getElementById('reset-dongle');
var restartServerBtn = document.getElementById('restart-server');
var restartNote = document.getElementById('restart-note');
var volumeSlider = document.getElementById('volume');
var vv = document.getElementById('vv');
var vsyncBox = document.getElementById('vsync');
var captureFormatSelect = document.getElementById('capture-format');
var resolutionSelect = document.getElementById('resolution');
var quality = document.getElementById('quality');
var qv = document.getElementById('qv');
var bar = document.getElementById('bar');
var statsToggle = document.getElementById('stats-toggle');
var gamepadSelect = document.getElementById('gamepad-select');
var gpDebugEl = document.getElementById('gpdebug');
var invertRyBox = document.getElementById('invert-ry');
var lStickDeadzone = document.getElementById('lstick-deadzone');
var lStickRange = document.getElementById('lstick-range');
var rStickDeadzone = document.getElementById('rstick-deadzone');
var rStickRange = document.getElementById('rstick-range');
var lStickDiagonal = document.getElementById('lstick-diagonal');
var rStickDiagonal = document.getElementById('rstick-diagonal');
var ltThresholdSlider = document.getElementById('lt-threshold');
var rtThresholdSlider = document.getElementById('rt-threshold');
var ltv = document.getElementById('ltv');
var rtv = document.getElementById('rtv');

var versionEl = document.getElementById('ver');
if (versionEl) versionEl.textContent = 'v' + C2C_VERSION;

var pc = null;
/* Which WebRTC client this page is, as the host named it in the answer
 * to /offer. Sent back on the polls this page already makes, and in the
 * goodbye below.
 *
 * It exists because the host cannot tell on its own: a browser that
 * goes away leaves the connection reading "connected" on the server for
 * as long as it runs, so a page that does not say it is leaving is
 * never noticed leaving. Each one that was missed kept an encoder
 * branch alive, and enough of them took the stream down for everybody
 * until the program was restarted. */
var clientId = null;

function log(s) {
  statusEl.textContent = s;
}
