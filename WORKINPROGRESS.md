# Capture2Cloud -- work in progress

Working notes: current state, known limitations, goals, and the
technical findings behind decisions that aren't obvious from the code.
For what the project is and how to install it, see [README.md](README.md).

Low-latency HDMI/USB capture for screen sharing/cloud gaming: V4L2 capture
+ SDL2 rendering + PulseAudio audio in a native window, with a web
streaming mode (WebRTC) to watch/play remotely from a browser, and a
gamepad bridge to relay input all the way to a real console via a
ConsoleTuner adapter (Titan One).

## Current state (2026-08-28)

### Working
- Native SDL2 capture + display, docked GTK control window (menu to
  toggle the web server / port), auto-start config
  (`scripts/.env`).
- WebRTC streaming: VP8 video + Opus audio, multi-client (each client
  gets its own RTP payloader).
- **YUYV capture by default** (`CAPTURE_FORMAT` in the .env, `mjpeg`
  still selectable). Measured on the reference card at 1080p60 before
  committing to it -- benchmarks in the session history:
  - per frame, capture to the encoder's I420: **0.40 ms (YUYV) vs
    7.81 ms (MJPEG: 2.64 decode + 5.17 RGB->I420)**, a 19x reduction.
    YUYV is already YUV, so the conversion is only a chroma subsample
    with no colour-space maths.
  - sustained 30 s run: 1800/1800 frames on time, worst interval 19.3 ms
    against a 16.7 ms nominal -- the ~250 MB/s does hold over USB3.
  - whole-process CPU with streaming active: **112% -> 76%**.
  - the MJPEG figures are measured on a colour-bar test pattern, the
    easiest possible case for a JPEG decoder; real game content would
    widen the gap further, while YUYV's cost is content-independent.
  - the display path benefits too: SDL takes the card's bytes straight
    into an `SDL_PIXELFORMAT_YUY2` texture, so the GPU does that colour
    conversion and the CPU never touches RGB at all.
  - **MJPEG is kept, not deleted**, unlike the H264 chain below: a
    Raspberry Pi 4 shares its USB3 bus with Ethernet and is unlikely to
    sustain raw 1080p60, so there the compressed path is the only way.
    `video_capture.c` also falls back automatically if a card refuses
    the requested format.
- **VP8 only.** A second, hardware-H264 chain used to sit alongside it,
  but the selection was `use_h264 = (vp8_pt < 0 && h264_pt >= 0)` -- it
  only ran when the browser offered NO VP8, which no WebRTC-capable
  browser does. It cost a permanent appsrc + encoder + parser + tee in
  the pipeline and a whole second colour conversion (RGB->NV12) kept
  alive for it. Removing it deleted that conversion outright. If
  hardware encoding is ever wanted back (a Raspberry Pi, say), it should
  return as a *preferred* path chosen by capability, not as dead code
  behind an unreachable condition.
- **Split into modules**: `capture2cloud.c` was a 755-line catch-all;
  V4L2/MJPEG now lives in `video_capture.c` and the PulseAudio thread
  with its hum filtering in `audio_capture.c`, leaving ~400 lines of
  orchestration. Both expose an opaque handle instead of sharing file-
  scope globals.
- Web page (`page.html`/`app.js`): volume control (up to 400% via Web
  Audio, but **the Web Audio API is avoided below 100%** — it cuts sound
  on Chromium for this WebRTC stream, cause not understood), vsync, video
  quality, collapsible stats (video/audio/gamepad), explicit gamepad
  selection when several are detected by the browser.
- Gamepad bridge (`gamepad_bridge.c`): browser Gamepad API →
  WebRTC DataChannel → dedicated thread → USB (libusb) → Titan One →
  console, Xbox 360 emulation. **Latency judged "perfect" by the user**
  with the current send loop (see "Key technical findings" below) — do
  not touch `send_thread_main()`'s loop structure without testing
  before/after very carefully, latency is particularly sensitive there.
  Real-controller passthrough merge exists in the code
  (`read_real_controller_state()`) but is currently **disabled** — see
  below; adjustable per-trigger thresholds (LT/RT) and a right-stick
  up/down invert toggle are exposed on the web page, since some gamepads
  (e.g. Xbox Series X|S observed here) expose LT/RT as extra axes instead
  of standard buttons.
- **Virtual on-screen gamepad** (`gamepad-select` dropdown → "Virtual
  buttons (touch)"): a transparent, multitouch, RetroArch-style overlay
  (Pointer Events, per-control drag capture) with Xbox/PlayStation/
  Nintendo skins, adjustable opacity/accent color, symmetric/asymmetric
  stick layout, and a free "move buttons" repositioning mode (saved in
  `localStorage`). A real gamepad, if connected, stays active
  simultaneously and is additively merged with touch input (see
  `combineVirtualGamepadState()` in `app.js`) — the on-screen
  buttons/sticks also visually reflect real-gamepad input even without
  any touch.
- **Per-gamepad rebind profiles** (`gamepad-select` → pick a real
  gamepad → "rebind controls"): which browser button/axis index feeds
  each `GAMEPAD_XB360_*` slot is no longer hardcoded -- it's a profile
  stored per gamepad id (`settings.gamepadProfiles[gp.id]` in
  `localStorage`), editable via a "click listen, then press/move the
  input" panel (`buildStateFromGamepad()` in `app.js`). A gamepad never
  rebound behaves exactly as before (the default profile is the
  previously-hardcoded mapping). Motivated directly by this session: the
  Xbox Series X|S controller here needs its own X/Y compensation; a PS5
  DualSense needs none (confirmed on real hardware — see the correction
  under "Key technical findings").
- **Keyboard/mouse mode** (`gamepad-select` → "Keyboard/mouse"): keys
  drive the d-pad, left stick (WASD-style digital extremes) and every
  button; the mouse (Pointer Lock, click the video to engage) drives the
  right stick continuously with a self-centering decay. Merges
  additively with a real gamepad too, same philosophy as the virtual
  overlay. Bindings are saved as named profiles
  (`settings.keyboardProfiles`), switchable on the fly from the same
  rebind panel used for gamepad rebinding, with "save as new profile"
  and "reset to default". **Not yet live-tested** (built and exercised
  under Node with a DOM/Gamepad API stub -- see "Key technical
  findings" -- while away from a browser to click through it by hand).
- **Nothing machine-specific is compiled in any more** (`app_config.c`):
  the capture device, audio source, adapter USB ids, web port,
  autostart, client limit and player password all come from
  `scripts/.env`, read fresh at each lookup so most edits apply without
  a restart. `MAX_CLIENTS` is clamped to a hard ceiling of 32 (the slot
  array is sized for it) so a typo degrades to a limit instead of an
  out-of-memory. Paths resolve from the **running executable**
  (`/proc/self/exe`), replacing the previous `__FILE__` (the compile-time
  source path -- wrong as soon as the binary moved) and hardcoded
  `$HOME/scripts/capture2cloud/...` (wrong for any other checkout).
  Verified by copying the whole directory to `/tmp`, renaming both the
  directory and the binary, and launching it from `/`: it found its own
  `page.html`, `app.js` and `.env` correctly.
  - The old second config source, `~/.config/capture2cloud.conf`, is
    gone: it silently took precedence over the `.env`, so changing
    `WEB_PORT` appeared to do nothing -- two files disagreeing about one
    setting is the exact surprise this work was meant to remove. Its two
    keys (`web_port`, `web_enabled`) are now `WEB_PORT` / `WEB_AUTOSTART`
    in the `.env`.
- **The front-end is served from disk**, not from the binary:
  `send_static()` in `web_stream.c` reads `page.html`/`app.js` (located
  relative to that file's own `__FILE__` path, so the working directory
  doesn't matter) on each request. **Editing the front-end no longer
  needs a recompile** -- save the file, refresh the browser. The binary
  therefore needs those two files present next to `web_stream.c`;
  missing/unreadable ones give a 404 plus an explicit
  `web_stream: cannot read <path>` on stderr. There is deliberately no
  compiled-in fallback copy (the previous `HTML_PAGE_BODY`/`APP_JS_BODY`
  arrays, ~2100 lines, are gone): a second copy would drift out of sync
  and could silently serve a months-old page the day the files went
  missing, which is a far more confusing failure than an obvious 404.
  No in-process caching either -- always re-reading is what makes
  edit-then-refresh work, and it costs nothing since the kernel's page
  cache already keeps these few-tens-of-KB files in RAM after the first
  read (an explicit cache, in `/dev/shm` or otherwise, would just
  duplicate what the page cache does for free).
- **Test suites** -- `./tests/run_all.sh` runs everything (C + JS) and
  exits non-zero if anything fails. **Run it after any change.**
  - **C unit tests** (`tests/c/`): each file `#include`s the `.c` under
    test so its `static` functions are reachable, and stubs whatever it
    doesn't need (no device is opened, no GStreamer pipeline built).
    `test_gamepad_bridge.c` pins down the GCAPI wire format --
    64 bytes, no leading report-id, LE16 length, payload at offset 4,
    tail zeroed -- i.e. exactly the layout whose corruption caused the
    original "USB accepts everything, console reacts to nothing" bug,
    plus `clamp_i8` saturation (an unclamped 200 wraps to -56, which
    would move a stick the wrong way). `test_web_stream_auth.c` covers
    the login path: constant-time comparison (prefixes and length
    differences must be rejected), token format/uniqueness, `.env`
    parsing for `PLAYER_PASSWORD` (quotes, comments, `=`/`#` inside the
    value, CRLF, similarly-named keys, duplicates), the session table
    including wraparound past capacity, and `request_may_control()` in
    both the open and password-protected configurations. It rewrites
    `scripts/.env` while running, so it saves the real one aside and
    restores it before exiting.
  - **Browser tests** (`tests/browser/`, Playwright + Chromium, run
    separately from `run_all.sh` because they need the app running with
    its capture device and are slower/flakier than the unit suites):
    - `node tests/browser/smoke.js` — the integration check the unit
      tests structurally cannot do: page loads, **WebRTC actually
      negotiates**, video frames really arrive and keep arriving, the
      gamepad DataChannel opens, no JS exception. This is precisely the
      class of failure that the reverted STUN change caused ("stuck on
      connecting" while every unit test still passed).
    - `node tests/browser/ui_shots.js` — renders the UI at phone-
      landscape and desktop sizes and writes screenshots, plus reports
      every on-screen control's position and flags overlaps/off-screen
      controls. Meant for *looking at* the layout rather than inferring
      it from CSS.
    - Setup, once: `npm install` in `capture2cloud/`, then `npx
      playwright install chromium`.
    - **Each run consumes one of the 8 `MAX_CLIENTS` slots for good**
      (see "no cleanup on client disconnect" below), so after a handful
      of runs `/offer` starts returning 400 and the app must be
      restarted. Annoying enough during browser testing that fixing the
      cleanup is now worth more than it looked.
  - **JS suite** (`tests/run_tests.js`, standalone: `node
    tests/run_tests.js`): executes the real `app.js` under Node against
    a minimal DOM/localStorage/Gamepad API stub (`tests/dom_stub.js`),
    covering keyboard mode, gamepad/keyboard rebind profiles,
    per-controller profile isolation, the virtual-overlay merge, video
    filters, settings persistence/migration, and the viewer/player UI.
    Catches what `node --check` can't (ordering bugs, undefined refs,
    logic errors).
  - Both suites were **verified to actually fail when the bugs they
    guard against are reintroduced** (the `saveSettings` in-memory
    desync for JS; a prefix-accepting password comparison and the
    report-id byte for C) -- worth re-checking that way for any new test
    added, since a test that can't fail is worse than no test.
- **Versioned settings schema**: `SETTINGS_VERSION` + `migrateSettings()`
  in `app.js`. Bump the version and add a migration step whenever the
  *shape* of a stored value changes (renamed key, number becoming an
  object, profile format change); simply adding a new key needs no bump,
  since every reader already defaults when its key is absent. Storage
  from a newer build, or corrupt JSON, is discarded rather than
  misread -- losing UI preferences is cheap, running on malformed state
  isn't.
- **Advanced video adjustments** (brightness/contrast/saturation/hue
  sliders + a reset button, in `controlsRow`): applied via the CSS
  `filter` property on both `<video>` and `<canvas>` (whichever vsync
  makes visible), so it's composited by the browser/GPU with no per-
  frame JavaScript work -- effectively free latency-wise, unlike a
  canvas/WebGL reprocessing approach. Persisted per-browser like every
  other setting on the page.
- **Viewer by default, log in to play**: anyone who opens the page can
  watch; driving the console requires logging in. The password lives in
  `capture2cloud/scripts/.env` as `PLAYER_PASSWORD` — the same
  git-ignored file as the Home Assistant credentials, so all of this
  project's secrets sit in one place. **Currently set to `changeme`.**
  Leaving it empty (or removing the key) reopens control to everyone,
  i.e. the pre-login behavior. It's re-read on every request, so
  changing it takes effect without restarting capture2cloud.
  - The enforcement is **server-side**, in `on_gamepad_message()`
    (`gst_webrtc.c`): each `WebrtcClient` carries a `may_control` flag
    and a viewer's gamepad messages are dropped there. Hiding the
    gamepad UI browser-side is cosmetic only — devtools can write to the
    DataChannel directly.
  - Flow: `POST /login` (body = password) returns a 64-hex-char session
    token from `/dev/urandom`, kept in memory server-side only (dies with
    the process) and in the tab's `sessionStorage` client-side
    (deliberately *not* in the persisted settings blob). `app.js` sends
    it as an `X-Player-Token` header on `POST /offer`; `may_control` is
    decided **once there**, at negotiation time, and baked into the
    client — never re-checked per gamepad message, so the input hot path
    keeps its latency. `GET /auth-status` ("required"/"open") tells the
    page whether to show the login button at all.
  - Logging in reconnects (`pc.close()` + `retry()`): the viewer/player
    decision belongs to the connection, so an existing one would stay a
    viewer forever.
  - Hardening: constant-time password/token comparison (a plain
    `strcmp` leaks how many leading chars matched, which is enough to
    recover a password one char at a time), and a 0.5s delay on each
    failed login to throttle guessing. Both password and token travel in
    the clear over plain HTTP — fine on the LAN, but the remote setup
    should be HTTPS (which the Cloudflare/NPM front implies anyway).
  - **Orthogonal to Authelia**: Authelia would gate who reaches the site
    at all; this gates who can *play* among those who did.
  - Server side verified end-to-end (`open`/`required` status, 401 +
    delay on a wrong password, distinct tokens, and the log line
    `web_stream: new client connecting as PLAYER|viewer` proving the
    flag is plumbed correctly). **The browser-side flow — login button,
    reconnect-as-player, and a viewer's input actually being ignored by
    the console — is still untested with a real browser.**
- **Wake the console from sleep**: cutting and restoring its power
  reliably wakes a Switch (this session's finding); nothing in the
  implementation is Switch-specific. `capture2cloud/scripts/
  wake_console.sh` (standalone, not compiled in) power-cycles a smart
  plug via Home Assistant's REST API, polling until each state change
  is confirmed (off, then hold ~2s, then on) or turning it straight on
  if it was already off. Configured entirely through
  `capture2cloud/scripts/.env` (`HA_URL`, `HA_TOKEN`, `HA_PLUG_ENTITY`
  -- git-ignored, never commit real values there) so swapping the plug/
  service never touches compiled code. Triggered manually (never
  automatic) from two places: a "Wake console" GTK menu item
  (`g_spawn_async`, non-blocking) and a "wake console" button on the web
  page (`POST /wake` -> `handle_wake()` in `web_stream.c`, backgrounds
  the script and replies immediately). **The web endpoint is
  players-only** (403 for a viewer): cutting the console's power is at
  least as disruptive as pressing its buttons, so it sits behind the
  same token gate as gamepad input, enforced server-side rather than
  only by hiding the button. The GTK menu item has no such check -- it's
  driven from the machine's own screen. Tested end-to-end on a stand-in
  plug (a video projector) -- real trigger on the console's actual plug
  still to be confirmed once it's wired up.
- **Fixed: "Select stuck permanently pressed"** (was also the cause of
  the phantom press seen right at startup, before any browser even
  connects). Confirmed root cause: `read_real_controller_state()` reads
  the real controller port at an unverified offset
  (`GCAPI_REPORT_INPUT_OFFSET`, from a GIMX pcprog.c comment) into
  `g_real_state`, which `send_thread_main()` used to add on top of the
  browser's virtual state. A single misaligned/garbage read landing a
  nonzero byte on the Select index, combined with the fact that a
  failed/short/malformed read used to leave `g_real_state` untouched
  (never cleared back to zero), meant one bad read could latch Select
  "pressed" forever afterwards — any further real press/release was
  invisible to the game since, from its point of view, the button never
  went back up. Fixed by (1) building `output[]` from the virtual/browser
  state only for now (real-controller merge disabled), and (2) making
  `read_real_controller_state()` always zero `g_real_state` on any
  failed/short/malformed read instead of keeping a stale value, for
  whenever the merge is re-enabled. `read_real_controller_state()` is
  still called every loop iteration to keep `send_thread_main()`'s exact
  USB transfer timing (the "perfect latency" checkpoint untouched). User-
  confirmed fixed in-game.

### Known broken / limited
- **Real-controller passthrough is disabled** (see the fix note above) —
  the console currently only reacts to the browser/virtual gamepad, not
  to a real controller plugged into the Titan One's controller port.
  Re-enable the merge in `send_thread_main()` only once
  `GCAPI_REPORT_INPUT_OFFSET` has been verified against this exact
  firmware (see Goals below) — and when doing so, still don't touch
  `send_thread_main()`'s loop structure (no sleep/condvar with a fixed
  delay), since that is exactly what caused the latency regressions
  during this session.
- ~~No cleanup of `WebrtcClient`/webrtcbin on client disconnect~~ —
  **done.** `teardown_client()` releases the tee pads and removes the
  client's elements from the live pipeline; `on_connection_state_notify`
  schedules it on FAILED/CLOSED, and `reclaim_dead_clients()` also
  accepts DISCONNECTED but only under slot pressure. The settings bar
  shows `viewers n/max`, fed by `GET /clients`.

## Goals going forward

1. **Verify the passthrough-merge byte offset, then re-enable the merge**
   (real-controller passthrough is fully disabled right now): determine
   the real offset of `GCAPI_REPORT` inside a `GPPKG_INPUT_REPORT` report
   via a targeted USB capture (press ONE known button at a time, compare
   which bytes change — same method that found the original write bug;
   see the `/tmp/titan_gtuner_capture.pcapng` capture generated during a
   real GTuner + MaxAim DI session as a reference, if still available).
   Once the right offset is confirmed, adjust `read_real_controller_state()`
   and put the `g_real_state` addition back into `send_thread_main()`'s
   `output[]` computation, **without changing the loop's structure**
   (no sleep/condvar with a fixed delay — the current latency comes
   specifically from there being no artificial pause anywhere in that
   loop).

2. **Control from the capture app itself, as an option**: be able to
   drive the virtual gamepad directly from `capture2cloud` (local SDL/GTK
   window) rather than only through the browser -- to play/test without
   going through the web stream. To design: read a local gamepad (via
   SDL_GameController?) and call `gamepad_bridge_update()` directly,
   toggleable from the GTK menu in addition to (not instead of) the
   browser path.

3. **Remote access (Nginx Proxy Manager + Authelia + Cloudflare) needs a
   STUN server, on both ends.** `app.js` creates
   `new RTCPeerConnection({ iceServers: [] })` -- no STUN/TURN at all --
   and `webrtcbin` isn't given one server-side either
   (`gst_webrtc_stream_handle_offer()` in `gst_webrtc.c`). This works
   today only because tests happen on the same LAN (both peers exchange
   host candidates directly). The HTTP signaling itself (`page.html`,
   `/app.js`, `POST /offer`, `POST /quality`) is plain HTTP, no
   WebSocket -- it should pass through Nginx Proxy Manager/Authelia/
   Cloudflare without any special config. The actual media (video/audio
   RTP + the gamepad DataChannel) is a separate direct peer-to-peer ICE/
   UDP connection that never goes through that HTTP chain at all, so a
   remote (non-LAN) client will very likely fail to connect without a
   STUN server on both sides.
   - Confirmed: home network here is a normal NAT (not CGNAT) -- STUN +
     a forwarded UDP port range should be enough, no TURN relay needed.
   - A first attempt (public STUN in `iceServers` + `stun-server` on
     `webrtcbin` + bounding the ICE port range via `ice-agent`'s
     `min-rtp-port`/`max-rtp-port`) was tried and **reverted**: the
     connection got stuck on "connecting" even on the LAN test setup
     that previously worked fine. The individual pieces were verified
     working in isolation (a standalone test program confirmed
     `stun-server` and `min-rtp-port`/`max-rtp-port` behave as expected
     on a bare `webrtcbin`), so the regression is likely an interaction
     with something else in this pipeline (multi-client handling,
     `bundle-policy`, or the per-client `GMainContext` push/pop around
     webrtcbin creation) rather than those properties being wrong in
     themselves -- needs closer investigation (e.g. add one piece at a
     time -- STUN alone first, then the port range -- and check
     `ice-gathering-state`/`ice-connection-state` transitions) before
     retrying, rather than reapplying the same combined change.

4. **Make mouse-driven stick movement feel more analog, less robotic**:
   the keyboard/mouse mode above already has a basic self-centering
   decay (`MOUSE_DECAY = 0.85` per frame in `app.js`) rather than a raw
   1:1 mapping, but it hasn't been tuned or evaluated with an actual
   controller in hand yet -- sensitivity/decay constants are current
   first guesses. Also still open: whether identical repeated stick
   value sequences from keyboard input (as opposed to mouse) could look
   bot-like to some anti-cheat systems, and whether that's worth adding
   deliberate small variance for. **Needs hands-on tuning once testable**
   rather than more speculative code changes.

5. **Audio latency: what is left after measuring the whole chain.**
   Budget as measured end to end:
   - PulseAudio capture: **22 ms -> ~1.2 ms**. Local playback shared the
     capture thread, and its blocking `pa_simple_write()` paced the
     whole loop; the same code with playback disabled reached 1.8 ms.
     Moving playback to its own thread behind a drop-oldest ring
     (`PlaybackRing` in `audio_capture.c`) removed it without losing the
     feature. Note the intuitive fix -- a *bigger* playback buffer --
     made it worse (34 ms), because the loop then paced itself on
     playback consumption instead.
   - GStreamer: ~0 ms measured. Queue capped at 2 buffers, Opus at 5 ms
     frames with `audio-type=restricted-lowdelay`.
   - Browser jitter buffer: 31-42 ms, `playoutDelayHint = 0` applied and
     confirmed supported.
   - Browser audio output device: 33 ms.
   The two browser-side items (~65 ms) now dominate and are largely out
   of reach. Also worth recording: **there is no continuous clock
   drift** -- Chrome's NetEq removed 5469 samples during startup and
   then exactly 0 across four 15 s windows, so the "delay grows over
   time" theory is disproven; what is heard early on is NetEq
   accelerating to drain the startup backlog.
   - Not worth doing: rewriting capture onto PulseAudio's async API.
     Measured directly, `pa_simple` as configured here reaches 3.5 ms
     while the async API with `PA_STREAM_ADJUST_LATENCY` managed only
     9.1 ms at best -- the rewrite would have been a regression.

6. **Reduce the audio lag behind video/inputs**: gamepad inputs and
   video currently feel ahead of audio by a small but noticeable margin.
   To investigate: (a) `RTCRtpReceiver.playoutDelayHint` on the audio
   receiver (Chrome's audio jitter buffer often targets a higher delay
   than necessary for smoothness; hinting it down trades a little more
   crackle risk for lower latency), (b) the GStreamer pipeline's audio
   side specifically -- Opus encoder frame size/ptime and any
   `rtpjitterbuffer latency` property that might default higher than the
   video path's equivalent, and (c) whether the PulseAudio simple-API
   capture buffer itself (`web_stream.c`/capture side) is configured
   with more inherent buffering than the V4L2 video capture path. Needs
   profiling on both ends to find exactly where the extra audio delay is
   introduced before picking a fix.

## Key technical findings (so they don't need rediscovering)

- **`saveSettings()` never updated the in-memory `settings` object,
  only `localStorage`** -- harmless for every setting that existed
  before this session (each one is read from `settings.x` exactly once,
  at page load, into a DOM element/JS variable that becomes the live
  source of truth from then on) but a real bug for the gamepad/keyboard
  rebind profiles, which re-read `settings.gamepadProfiles`/
  `settings.keyboardProfiles` on every access: a rebind would save to
  `localStorage` correctly but the very next read would still see the
  pre-rebind (stale in-memory) value, silently discarding it. Fixed by
  having `saveSettings()` also mirror the patch into the live `settings`
  object. Caught by actually executing `app.js` under Node with a
  minimal DOM/`localStorage`/Gamepad API stub and running through
  rebind scenarios end-to-end (mode switch, key/button capture, save,
  re-read) -- worth doing again for any future feature that, like this
  one, re-reads `settings.x` repeatedly instead of once at load, since
  nothing else in this codebase does that and the bug is easy to miss
  by inspection alone.

- **Actual GCAPI HID report format on the wire**: 64 bytes
  `[type][length LE16][first][data...]`, **without** the 0x00
  report-id byte that GIMX's C struct `s_gppReport` (pcprog.c) suggests
  (65 bytes) — GIMX automatically drops it when sending
  (`gusbhid_write_timeout` skips the first byte if it is `== 0x00`).
  Sending 65 bytes shifts the whole packet by one and the firmware
  silently ignores it (accepted by USB, no effect on the console).
  Confirmed by a USB capture (`tshark`/`usbmon`) of GTuner Pro's real
  traffic.
- **GCAPI ("Direct Input" mode) needs a one-time setup via GTuner Pro**
  (Windows, or a Windows VM — Wine failed, deep bug in its embedded
  browser component/TSF): without configuring the output profile (Xbox
  360) at least once via GTuner, neither real passthrough nor software
  injection produce any effect on the console, even though USB accepts
  everything without error. This configuration is stored on the device
  itself (persists across reconnections here). `Gtuner.exe`/`GtunerPro.zip`
  saved under `../gtuner/`.
- **The catastrophic latency observed** came from a pile-up
  (backpressure) between DataChannel messages (~60Hz) and blocking USB
  writes, then from a default GLib context shared with the GTK loop for
  each client's webrtcbin (see the push/pop of `g->ctx` around
  `gst_element_factory_make("webrtcbin", ...)` in
  `gst_webrtc_stream_handle_offer()`), and finally from a
  `KEEPALIVE_MS`/condition-variable wait in `send_thread_main()` that made
  even real passthrough follow an imposed pace. The current version runs
  a continuous loop with no artificial wait at all, paced only by the
  actual duration of the USB transfers.
- The Titan One can end up in a degraded state after many repeated USB
  connect/disconnect cycles (interface claim/release) — a real physical
  unplug/replug (not just restarting the program) resolved this several
  times during this session.
- On this particular Xbox Series X|S controller, the browser reports 18
  buttons and 6 axes (standard mapping normally has 17 buttons/4 axes):
  LT/RT come back as axes 4/5 (range -1.00 to 1.00) rather than standard
  buttons 6/7, and X/Y come back transposed — both compensated for in
  `app.js`.
- **Correction (confirmed on real hardware):** the X/Y transposition is
  NOT the GCAPI/console translation, as first assumed here — it is that
  controller misreporting its own buttons. Applying it to every gamepad
  is what made square and triangle come out swapped on a friend's PS5
  DualSense, which reports a proper standard mapping and needs no
  correction at all. `gamepadNeedsXYSwap()` now keys it on the
  controller (Microsoft vendor id / "xbox" / "xinput"); anything it
  guesses wrong stays fixable per-pad in the rebind panel. The likely
  root cause is the Linux xpad driver's button order, so the same pad on
  another OS may not want it either.

## Headless mode and hardware that comes and goes

- **`--headless`** (launcher: `toggle_capture2cloud_headless.sh`): no SDL
  window, no GTK control bar. `WEB_AUTOSTART=0` is ignored there -- the
  web page is the only interface, so refusing to start the stream would
  leave a process capturing into nothing. Measured on this machine with
  one client connected: **~27% CPU headless vs ~170-190% windowed**, the
  difference being the 1080p60 texture upload and present.
- Both launchers share `scripts/lib_toggle.sh` (one build recipe, one
  device check) and refuse to start while the other mode holds the
  capture card and the port.
- **The capture card can drop off the USB bus mid-session.** Observed:
  `VIDIOC_QBUF: No such device`, after which the program used to break
  out of its loop and exit -- taking the web server with it and leaving
  every viewer on a frozen last frame. The main loop now closes the
  device and waits for it to reappear (`access()` first, so a missing
  node costs one syscall rather than a failing `open()` every second),
  keeping the stream and the gamepad bridge alive. A card that comes
  back at a different resolution is refused with a message: the encoder
  and every negotiated client were built for the original size.
- **The adapter can be unplugged and plugged back in.** The send thread
  reconnects on its own (`usb_open_and_claim()` replays GTuner's startup
  handshake, which the adapter needs on every enumeration). The
  connected hot path is unchanged -- the two new branches are only taken
  while the link is down -- and a disconnect is logged once rather than
  per failed transfer, which in a loop with no sleep would be thousands
  of lines a second.
- **`gamepad_bridge_reset()`** re-enumerates the adapter. Called after
  the wake script finishes: the adapter last handshook with a console
  that was asleep, and coming out of standby is not enough for it to be
  seen again. Asynchronous by design -- the send thread owns the handle
  and is using it continuously, so the reset happens there rather than
  underneath it. The wake now runs on its own thread so the reset can
  wait for the script instead of racing it, and the plug stays off for
  3s (2s did not always register).

## Corrected: the GMainContext isolation never worked

`gst_webrtc_stream_handle_offer()` pushed `g->ctx` as thread-default from
the HTTP thread before creating each `webrtcbin`, to keep the gamepad
DataChannel off GTK's context. That cannot work:
`g_main_context_push_thread_default()` acquires the context, and
`gst_thread_main()` already owns it for the lifetime of its
`g_main_loop_run()`. GLib said so on every connection:

    g_main_context_push_thread_default: assertion 'acquired_context' failed

So the push was a no-op and `webrtcbin` captured the **global** default
context -- exactly what the code was trying to avoid. It is now built on
the gst thread itself, through a small synchronous invoke (GLib has no
`g_main_context_invoke_sync`), with a timeout so a client's request can
never hang on a context whose loop is not running. The assertions are
gone from the log.

## Hitches: measured, not guessed

Reported as a brief freeze every ~50s, watching locally, video only (the
sound never broke).

- **Not the capture card.** With a gap detector on the main loop
  (`FRAME_GAP_WARN_MS`, logs any wait over 50 ms together with how long
  the previous push took, so a starved card is distinguishable from a
  busy downstream), **zero gaps in 230 s**. The card and the loop deliver
  steadily.
- **It was the keyframe cadence.** `keyframe-max-dist=30` at 60 fps is
  two keyframes a *second*. Measured over 230 s, counting the interval
  between presented frames in the browser (`requestVideoFrameCallback` --
  `getStats()` once a second cannot see three missed frames out of 60):

  | keyframe-max-dist | hitches | frames dropped by the browser |
  |---|---|---|
  | 30 (0.5 s) | 57 | 487 |
  | **300 (5 s)** | **19** | **203** |
  | 600 (10 s) | 27 | 178 |

  A 1080p keyframe costs far more to decode than a P-frame, and at a
  fixed CBR budget it steals bitrate from the 59 frames around it. All of
  the gain is in leaving 30 behind; past 5 s it is noise, while recovery
  from a lost keyframe request only gets slower. Now `KEYFRAME_MAX_DIST`
  in the .env, default 300.

## The local window was delaying the stream

`SDL_RenderPresent()` blocks until the display's next vsync, and it ran
*before* the frame was handed to the encoder -- so in windowed mode every
frame waited on the local monitor, up to a full refresh period, before it
started travelling to the viewer. The push now comes first and the window
second: the person playing over the network is the one who cares about
latency, and a monitor can be one frame behind.

Headless was never the slower mode. Browser-side latency, same machine,
45 s each:

| | jitter buffer | decode | total (received -> ready) |
|---|---|---|---|
| headless | 25.6 ms | 6.99 ms | **32.6 ms** |
| windowed | 24.0 ms | 7.59 ms | **31.6 ms** |

Identical within noise. What changes in headless is that the local SDL
window -- a near-zero-latency view of the same capture -- is gone, so the
browser's ~32 ms plus display latency is no longer being compared against
an instant reference sitting next to it.

## The adapter was being killed before it could be handed back

Input latency that appeared after a quit/relaunch cycle and that only a
physical unplug/replug cleared. Measured rather than guessed:

- A clean shutdown took **2003 ms**. The launcher escalated to SIGKILL at
  **2000 ms**. `gamepad_bridge_shutdown()` ran *last*, after the audio
  threads, the sockets and the GStreamer pipeline -- so the
  `LEAVE_CAPTURE` report that hands the adapter back was routinely never
  sent, and it stayed in capture mode across the relaunch. Its startup
  handshake (F0 / LEAVE / F1 / ENTER) does not clear that state; only a
  re-enumeration does, which is why replugging worked.

Four changes, each addressing a different part of it:

1. **The adapter is shut down first.** Nothing else in the teardown is
   time-sensitive -- the pipeline and sockets are this process's own,
   while the adapter is hardware left in a state for whatever runs next.
2. **`libusb_reset_device()` on open**, before the handshake: the replug,
   done in software. A run that was SIGKILLed or crashed no longer
   poisons the next one. Retried once, since a reset can hand back an
   invalidated handle if the device comes back re-enumerated.
3. **USB transfer timeout 1000 ms -> 250 ms.** Only ever reached when the
   adapter does not answer, so the send loop's timing is untouched; it is
   what bounded how long shutdown waited to join that thread.
4. **Launcher grace 2 s -> 5 s**, so a clean shutdown has room even if it
   gets slower later.

Result: shutdown is now **1004 ms**, and the log says
`gamepad_bridge: left capture mode (ok)` -- confirmed over three
quit/relaunch cycles.

## One instance, found by looking at processes

Deleting a pid file used to orphan a running instance: nothing could find
it, the other launcher happily started, and the second copy failed with
`video_capture: device accepts neither YUYV nor MJPEG` -- a message that
sends you inspecting the capture card when the answer is that the device
was already open.

- `c2c_find_running <headless|windowed|any>` walks `/proc` and confirms
  each candidate's `cmdline` against the binary path, so it finds an
  instance with no pid file and ignores a stale file pointing at a
  recycled pid. Both the stop path and the refuse-the-other-mode check
  now go through it; the pid file is a convenience, not the truth.
- Running a launcher again therefore stops an orphan instead of leaving
  it lingering invisibly.
- `open_device()` reports `EBUSY` for what it is, instead of letting it
  fall through the format-negotiation retry and blaming the card.

## Complexity pass

No algorithmic problems to find: every structure here is small and
bounded (32 client slots, 21 gamepad fields, 16 sessions), the SDP scan
is a single linear pass, and the per-frame `sws_scale` is inherent. What
did turn up was constant-factor waste on the paths that run 60 times a
second or on every input event.

- **`saveSettings()` wrote synchronously on every `oninput`.** It parsed
  the whole stored blob, merged, re-serialised and called
  `localStorage.setItem` -- which is synchronous -- and ten handlers call
  it (volume, both trigger thresholds, overlay opacity and colour, the
  four video filters, quality). `oninput` fires on every pixel of a drag,
  and the gamepad loop shares that thread, so dragging a slider added
  input latency. The in-memory update stays immediate (rebind profiles
  are re-read from `settings` at runtime); only the write is coalesced,
  with a flush on `pagehide`/`visibilitychange`. Benchmarked on a 3.9 KB
  blob: a 500-event drag went from 28.5 ms to 1.3 ms, **22x**, and more
  in a real browser where `setItem` is not a stub.
- **The gamepad debug dump was built every frame even when hidden** --
  two `map`+`join` passes, a `toFixed()` per axis and a DOM write, 60
  times a second, discarded while the stats row is collapsed (its default
  state). Now built only when visible.
- **`getGamepadProfile()` rebuilt the defaults every frame.** A pad that
  has never been rebound -- the normal case -- allocated 22 objects plus
  a lowercased copy of its id per frame to read four button indices. Now
  cached per controller id. That makes the object shared, so
  `applyGamepadBinding()` copies before writing: without that, the first
  rebind would quietly rewrite what "default" means for that pad. Covered
  by a test that fails if the copy is removed.

## Security pass

`tests/security/attack.js` fires the traffic at a running instance (raw
sockets, since several probes are malformed in ways an HTTP client would
refuse to send) and is wired into `run_all.sh`, skipped when nothing is
listening. It never completes a `/wake` with a valid token -- that
power-cycles the console's mains supply, and the refusal path is what
matters.

Held up: path traversal in eleven encodings (the router is an exact-match
whitelist, so no filesystem path is ever built from input), access
control on `/wake` and `/quality` with absent, bogus and oversized
tokens, header injection, malformed and overflowing `Content-Length`,
8 KB paths, 9 KB header lines, 200-header requests, NUL bytes, method
confusion, and 60 simultaneous connections. Buffer sizes check out
(`%7s`/`method[8]`, `%255s`/`path[256]`, `%64s`/`tok[65]` -- exact), and
`secure_equals` has no early exit.

**One real weakness found and fixed: the login delay did not survive
parallelism.** The 0.5 s penalty blocked only the guessing connection's
own thread -- 30 guesses fired at once finished in 505 ms instead of 15 s,
about 60 attempts a second against a default password of `changeme`.
Failures are now counted server-wide: five stop login attempts for
everyone for 30 s, and a correct password is refused during a lockout too
(otherwise guessing straight through it would still work). Re-measured:
30 parallel attempts now yield 5 evaluated and 25 refused with 429.
Someone can use this to lock the real user out in half-minute bursts,
which is the better half of the trade for a LAN/Tailscale service.

### Not fixed, deliberately

- **The `Host` header is not validated**, so DNS rebinding is possible: a
  page the user visits could resolve a name it controls to this machine
  and then talk to the server as same-origin. Fixing it means an
  allowlist, which would break access by IP, hostname and Tailscale name
  unless each is listed -- a decision about deployment, not a bug to
  silently patch.
- **With no `PLAYER_PASSWORD` set, `/wake` and `/quality` take any
  request**, including a cross-origin POST from any site the user
  happens to visit -- which would power-cycle the console. That open mode
  is deliberate ("exactly how it behaved before login existed"), so the
  answer is to set a password rather than to change the mode.
- **HTTP is unencrypted.** Passwords and session tokens cross the LAN in
  clear. Over Tailscale the tunnel covers it; on a plain LAN it does not.

## Freezes: what changed, and what is still open

Reported: freezes persist, worse on complex or abruptly changing scenes,
and suspected to involve the YUYV capture path.

- **The capture format is now switchable while running**, from the page
  (`capture` dropdown) and over `POST /capture-format` (`yuyv`/`mjpeg`,
  player-gated; `GET` reports what the device is *actually* opened with,
  since the driver may refuse a format and fall back). Comparing the two
  needs hours of real use, and editing the .env and restarting was too
  much friction for that. The capture loop performs the switch, because
  it owns the mapped buffers; the HTTP thread only sets a flag. Verified
  both directions with a client watching: ~58 fps before and after, the
  stream survives the ~220 ms close/reopen.
- **The video appsrc queue was unbounded.** appsrc defaults to
  `max-bytes=200000` with `leaky-type=none`, and a 1080p I420 frame is
  **3.1 MB** -- fifteen times that limit. The limit only drives the
  need-data/enough-data signals, which nothing here listens to, so with
  `block=false` every frame was accepted anyway and the internal queue
  grew without bound whenever the encoder fell behind. On a complex or
  abruptly changing scene that is exactly when it does: the encoder then
  worked on older and older frames while memory grew 3.1 MB at a time.
  Now `max-buffers=1 leaky-type=downstream` -- for a live source the
  newest frame is the only one worth having.

  **Not proven to be the freeze.** It is a real defect and it matches
  "worse on complex scenes", but the one stall actually captured showed
  `recv=0, kB=0` for a whole second, which is a producer stopping rather
  than a backlog. This fix bounds latency and memory under load; whether
  it removes the freeze needs the long session only the user can run.
- What to look for when it happens: the log line
  `video_capture: N ms gap before this frame (previous push took M ms)`.
  If it appears, the capture loop stalled. If the viewer sees a hole and
  that line does NOT appear, the stall is downstream of capture.

## Low-latency audio route removed

The opt-in Web Audio path (`latencyHint: 0` around the live MediaStream)
measured no better than letting the video element play the track, and
having two live output routes is what made "mute" silence only half the
sound. Gone: the checkbox, the graph, `setLowLatencyAudio()` and the
`lowLatencyAudio` setting. `applyVolume()` is back to a single route.

## Waking the console: the adapter needs a real gap, not a reset

`libusb_reset_device()` re-enumerates in milliseconds, and that turned
out not to be enough -- input latency after a wake persisted until the
adapter was physically unplugged for a couple of seconds. The reset now
reproduces that: reset, release, **stay closed for 3 s**
(`RESET_HOLD_MS`, since two was the shortest unplug observed to work),
then reopen. The wait is sliced (`interruptible_delay`) rather than one
`SDL_Delay`, because the send thread is joined by
`gamepad_bridge_shutdown()` before it hands the adapter back -- a
three-second block there would have re-created the very bug that made
quitting leave the adapter in capture mode.

## Waking the console: reset when the picture comes back, not before

Resetting the adapter as soon as `wake_console.sh` returned was too
early. The console is still around ten seconds from drawing anything at
that point; the USB link came back while it was not listening, and the
adapter never re-attached -- the gamepad did not come back at all.

The capture card is a UVC device that generates its own "no signal"
pattern, so V4L2 reports the input as `Status: ok` either way and
`VIDIOC_QUERY_DV_TIMINGS` is not even implemented. There is nothing to
query; what can be seen is the picture changing.

`video_capture_watch_for_change()` is armed once the wake script
finishes. Rather than encode what the no-signal pattern looks like, the
frame present at that moment becomes the reference -- whatever is on
screen while the console sleeps. A grid of 1024 samples across the
buffer is compared per frame; enough of them differing, for three
consecutive frames, means the console has started drawing. The capture
loop then resets the adapter. There is a timeout, so a wake that somehow
changes nothing still gets its follow-up.

Verified on real hardware, and the whole sequence fired exactly once:

    Titan One disconnected (LIBUSB_ERROR_NO_DEVICE)   <- the mains cut
    Titan One reconnected, capture mode active        <- hot-plug
    picture changed (1009/1024 samples), console is up
    wake: picture is back, re-enumerating the adapter
    resetting Titan One (held closed for 3000 ms)
    Titan One reconnected, capture mode active

Worth noting: the plug cycle physically drops the adapter off the USB
bus. The hot-plug support added earlier is what carries it through that,
and it did so unattended.

## Cumulative input latency: the loop was talking too much

Measured: **300 reports/s**, 0.66 ms per transfer, nearly all of them
identical -- nothing is moving most of the time. The send loop has no
pacing at all, so if the adapter accepts reports faster than the console
drains them, the surplus queues up *inside the adapter*: a 50/s gap is
twelve seconds of lag after a minute of play. No reset of the USB link
can drain a backlog that lives in the device, which is why resetting
never fixed it.

A report identical to the last one carries no information, so it is no
longer sent; a keepalive still goes out every 100 ms. Idle traffic went
from 300 to 10 reports/s -- 30x less -- with the transfer itself
unchanged at 0.85 ms average, 1.01 ms worst (measured with
`SDL_GetPerformanceCounter`; an earlier millisecond-resolution version of
the same counter reported a nonsensical 100 ms). Real input is untouched:
a changed state goes out on the very next pass. `GAMEPAD_DEDUP=0`
restores the old behaviour.

**Hypothesis, not a proven fix.** It removes the only plausible source of
a *cumulative* lag in a queueless design, but only a long session can
confirm it.

## Player-gated requests were missing their token

`/quality` and `/capture-format` were POSTed without `X-Player-Token`,
so the server answered 403 and the page silently did nothing. It made the
quality slider inert and made the capture-format toggle snap back -- the
read-back afterwards correctly reported that nothing had changed, which
looked like the switch failing when it was the request being refused.

Two of the three call sites got it wrong, so the header is no longer
each caller's job: `playerFetch()` adds it. A test asserts every gated
endpoint carries it.

Two things that fell out of the same work:

- **The SDL texture is rebuilt when the capture format changes.** It was
  created once at startup, so switching to MJPEG fed RGB24 bytes into a
  YUY2 texture: a green window, then a quarter of the picture (5760
  bytes per row read as 3840).
- **The security suite is now isolated from its own lockout.** It
  deliberately triggers the 30 s failed-login lockout, which outlives a
  run, so two runs back to back saw 429 where they expected 401 and
  reported a false failure. It now waits the lockout out first; verified
  by running it twice in a row.

## MJPEG is now the default

Switched at the user's request, on the suspicion that raw YUYV is behind
the long-session freezes. YUYV stays one click away and remains the
automatic fallback if the driver refuses MJPEG.

It is not free. Measured on this machine, same scene, two clients
connected:

| capture format | process CPU |
| --- | --- |
| MJPEG (decoded here) | **253%** |
| YUYV (raw) | **204%** |

About fifty points of a core, which is the JPEG decode. The trade is
deliberate: MJPEG moves a fraction of the bytes over USB, and finding out
whether that is what stops the freezes is worth more than the CPU.

The choice is remembered per browser (`settings.captureFormat`) and
re-applied on becoming a player. Note it is a **global** setting -- there
is one capture device -- so the last player to load the page wins. With
more than one player, pin it with `CAPTURE_FORMAT` in the .env instead,
which applies at startup and belongs to the machine.

## A "reset dongle" button

`POST /reset-dongle`, player-gated, running the same re-enumeration the
wake path triggers. Exposed because the adapter occasionally needs it
after the console has been handled in ways this program never sees. The
button says "resetting..." for four seconds, since the adapter is held
closed for three by design.

### A trap worth knowing about

The security suite deliberately triggers the 30-second failed-login
lockout. A `/login` issued right after a test run therefore returns 429,
and a script that captures its body as a token ends up sending an empty
one -- which then looks exactly like a broken endpoint returning 403.
That is what a `/reset-dongle` "failure" turned out to be during this
session. The suite now waits the lockout out at its own start; a manual
`curl` after a run still has to.
