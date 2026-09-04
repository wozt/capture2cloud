/*
 * capture2cloud -- viewer by default, player on request
 *
 * A viewer gets the picture and the sound and nothing else. The
 * distinction is enforced by the host on every gated endpoint; this
 * file only decides what is worth showing, since a menu that opens
 * onto five things you may not touch is worse than no menu.
 */

/* --- "viewer by default, log in to play" ---
 * Everyone can watch; driving the console needs a password. The token
 * below is what the server checks -- this UI only reflects that state,
 * it does not enforce it (the real gate is server-side, in
 * on_gamepad_message() in gst_webrtc.c: a viewer's gamepad messages are
 * dropped there, so bypassing this UI achieves nothing).
 *
 * The token is kept in `sessionStorage`, deliberately NOT in the
 * persisted settings blob: it should not outlive the tab, and it has no
 * business sitting in the same place as UI preferences. Server-side it
 * only lives in memory anyway, so it dies with the process too. */
/* Whether this page may drive the console. Starts false: /auth-status
 * decides, and until it answers there is nothing to send anyway. */
var playerControlsEnabled = false;

var authRow = document.getElementById('auth-row');
var authState = document.getElementById('auth-state');
var loginBtn = document.getElementById('login-btn');
var playerToken = null;

/* Every player-gated endpoint has to carry the session token, or the
 * server answers 403 and the page silently does nothing. Going through
 * one helper instead of remembering the header at each call site: it was
 * forgotten on /quality and /capture-format, which made the quality
 * slider and the capture-format toggle look broken -- the toggle even
 * snapped back, because the read-back afterwards correctly reported that
 * nothing had changed. */
function playerFetch(url, options) {
  var opts = options || {};
  var headers = {};
  for (var k in opts.headers || {}) headers[k] = opts.headers[k];
  if (playerToken) headers['X-Player-Token'] = playerToken;
  opts.headers = headers;
  return fetch(url, opts);
}
try {
  playerToken = sessionStorage.getItem('capture2cloud_player_token');
} catch (e) {}

/* A viewer is here to watch: sound is theirs to adjust, nothing else is.
 *
 * Everything gamepad-related would be dropped by the server anyway
 * (input, /wake and /quality are all refused without a player token), so
 * showing those controls only invites clicks that silently do nothing.
 * The video settings go too -- the bitrate genuinely is everyone's (ONE
 * shared encoder feeds every client, so a viewer lowering it degrades
 * the picture for whoever is playing), and the rest follow the same rule
 * to keep "viewer = mute and volume" simple to reason about.
 *
 * A distinct class from `hidden` on purpose: several of these controls
 * run their own show/hide logic (the virtual-pad rows, the rebind
 * button), and reusing `hidden` here would have this function reveal
 * controls that logic had deliberately hidden. */
function viewerHiddenControls() {
  /* Built at call time: some of these are declared further down the
   * file, and this only ever runs once the whole script has. */
  return {
    /* The groups go too: everything inside every one of them is a
     * player control, so leaving the headings would offer a viewer five
     * menus that open onto nothing. */
    bare: [wakeConsoleBtn, resetDongleBtn, restartServerBtn, rebindBtn, padTestBtn,
           vidFilterResetBtn, vgpResetBtn]
      .concat(menuGroups),
    /* Sitting inside a <label>: hide the label so its text goes too. */
    labelled: [gamepadSelect, vsyncBox, captureFormatSelect, resolutionSelect, fullscreenBox, quality,
               vidBrightness, vidContrast, vidSaturation, vidHue,
               invertRyBox, ltThresholdSlider, rtThresholdSlider,
               lStickDeadzone, lStickRange, lStickDiagonal,
               rStickDeadzone, rStickRange, rStickDiagonal,
               vgpLayoutSelect, vgpSymmetricBox, vgpColorInput, vgpOpacityInput, vgpEditBox]
  };
}

function setPlayerUi(isPlayer) {
  playerControlsEnabled = isPlayer;
  setGamepadLoopRunning(isPlayer);
  authState.textContent = isPlayer ? 'player' : 'viewer';
  authState.classList.toggle('player', isPlayer);
  loginBtn.classList.toggle('hidden', isPlayer);

  var c = viewerHiddenControls();
  c.bare.forEach(function (el) {
    if (el) el.classList.toggle('viewer-hidden', !isPlayer);
  });
  if (!isPlayer) {
    /* A panel left open would stay drawn over the video with its group
     * gone from the bar. */
    menuGroups.forEach(function (group) { group.open = false; });
  }
  c.labelled.forEach(function (el) {
    if (el && el.parentElement) el.parentElement.classList.toggle('viewer-hidden', !isPlayer);
  });

  if (isPlayer) {
    /* Whatever was plugged in while we were only watching has never been
     * looked at -- fill the dropdown now. */
    updateGamepadList();
    restoreCaptureFormat();
  } else {
    setVirtualGamepadActive(false);
    /* Both panels are gamepad tools; neither may stay open across a
     * demotion (a stale token found at connection time does exactly
     * that). */
    if (padTestEl) padTestEl.classList.add('hidden');
    if (rebindEl) rebindEl.classList.add('hidden');
  }
}

/* Asks the server whether a password is configured at all. If not
 * ("open"), the login UI stays hidden entirely and everyone can play --
 * i.e. exactly how this project behaved before login existed. */
fetch('/auth-status')
  .then(function (r) {
    return r.text();
  })
  .then(function (status) {
    if (status !== 'required') {
      authRow.classList.add('hidden');
      setPlayerUi(true);
      return;
    }
    authRow.classList.remove('hidden');
    setPlayerUi(!!playerToken);
  })
  .catch(function () {
    /* Can't tell: assume open rather than locking the user out of a
     * setup that has no password configured. */
    authRow.classList.add('hidden');
    setPlayerUi(true);
  });

loginBtn.onclick = function () {
  var password = prompt('Password to play:');
  if (!password) return;
  loginBtn.disabled = true;
  loginBtn.textContent = 'checking...';
  fetch('/login', { method: 'POST', body: password })
    .then(function (resp) {
      if (!resp.ok) {
        log(resp.status === 401 ? 'login: wrong password' : 'login: server error ' + resp.status);
        return null;
      }
      return resp.text();
    })
    .then(function (token) {
      if (!token) return;
      playerToken = token;
      try {
        sessionStorage.setItem('capture2cloud_player_token', token);
      } catch (e) {}
      setPlayerUi(true);
      log('logged in, reconnecting as player...');
      /* The viewer/player decision is made server-side when the
       * connection is negotiated, so an existing connection stays a
       * viewer forever -- reconnect to come back as a player. */
      if (pc) pc.close();
      retry();
    })
    .catch(function (e) {
      log('login: ' + e);
    })
    .finally(function () {
      loginBtn.disabled = false;
      loginBtn.textContent = 'log in to play';
    });
};
