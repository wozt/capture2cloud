# Capture2Cloud — work in progress

What the code does not say by itself: measured numbers, root causes, and
the decisions that look wrong until you know why. For what the project
is and how to install it, see [README.md](README.md).

> This is a summary. The full 1004-line journal it was condensed from is
> at `legacy/WORKINPROGRESS-full-2026-09-09.md` — kept on disk, ignored
> by git, and also recoverable from history
> (`git show 0abbbb3:WORKINPROGRESS.md`). Anything below that reads as a
> bare conclusion has its full working in there.

Low-latency HDMI/USB capture: V4L2 + SDL2 + PulseAudio in a native
window, streamed to a browser, and a gamepad bridge relaying input to a
real console through a ConsoleTuner adapter (Titan One). Three clients
read the stream: the browser (WebRTC **or** WebSocket, a live setting),
an Android app, and a Switch homebrew — the last two over the binary
protocol in `c2s_protocol.h`.

---

## The shape of it

| Piece | File | Note |
| --- | --- | --- |
| Orchestration | `capture2cloud.c` | ~400 lines; was a 755-line catch-all |
| V4L2 / MJPEG | `video_capture.c` | opaque handle, no shared globals |
| PulseAudio + hum filter | `audio_capture.c` | playback on its own thread |
| WebRTC | `gst_webrtc.c` | one payloader per client |
| HTTP / WebSocket | `web_stream.c` | exact-match route whitelist |
| WebSocket framing | `ws_frame.c` | RFC 6455, checked against its test vector |
| Native transport | `switch_stream.c` | routed by **stream**, not codec |
| Input → USB | `gamepad_bridge.c` | GCAPI over libusb |
| Config | `app_config.c` | everything from `scripts/.env` |
| Front end | `page.html` + `web/` | eleven scripts, served from disk |

**Three encodes, each fed only while watched**: the console's 720p
H.264, the native VP8, the browser's 1080p H.264. A path with no
audience costs nothing — 4% of a core idle against 18.5% with one
WebSocket client. The routing key inside the native transport is the
*stream*, not the codec, because two codecs cannot describe three
audiences: the browser's H.264 and the console's are the same codec at
different sizes.

**WebRTC serves VP8 only.** A hardware-H.264 chain used to sit beside it
behind `use_h264 = (vp8_pt < 0 && h264_pt >= 0)` — it ran only when a
browser offered *no* VP8, which none does. It cost a permanent
appsrc + encoder + parser + tee and a second colour conversion. If
hardware encoding is wanted back (a Pi), it returns as a *preferred*
path chosen by capability, not as dead code behind an unreachable
condition.

---

## The page's second transport

The browser can take the video over WebRTC or over a WebSocket carrying
the same messages the phone and the console read, decoded with
WebCodecs. Above the handshake **one binary frame is exactly one
protocol message** — the page reads the bytes the console reads.

**Why both.** WebRTC's media is peer-to-peer UDP and never touches the
HTTP chain: that is what keeps its latency honest, and also why it does
not survive a reverse proxy. The WebSocket is ordinary web traffic on
the same port, so Cloudflare, Nginx and Authelia relay it with nothing
explained to them and no STUN or TURN involved. The cost is TCP's and it
is real: a lost packet stalls what is behind it instead of costing one
frame, trading artefacts for pauses — the wrong way round for something
being played on. Hence a choice, not a replacement.

**How it is chosen.** `WEB_TRANSPORT` picks what the host *starts* on;
after that it is a shared, players-only setting in the page's menu.
Other pages follow within a poll of `/shared` rather than being left on
an encoder that has stopped.

**At most four of the eight client slots may be browsers**
(`SS_MAX_WS_CLIENTS`). Sharing one pool let a page reconnecting in a
loop fill it and leave the Switch and the phone refused with "the host
closed the connection". A browser has a second transport; a console does
not.

The session token travels in the WebSocket's **query string** — a
browser's `WebSocket` constructor takes no headers, and without it the
host could only ever see a viewer. Frames are dropped when the decoder
falls more than four behind, with a keyframe asked for at most once a
second: `decode()` queues rather than blocks, so a browser that cannot
keep up at 1080p60 grows a queue that never drains.

---

## Rules that cost something to relearn

- **Do not restructure `send_thread_main()`'s loop.** No sleep, no
  condvar, no fixed delay. A `KEEPALIVE_MS` condition-variable wait
  there is what made even real passthrough follow an imposed pace, and
  it is the single most latency-sensitive place in the project.
- **Access control is server-side or it is nothing.** Hiding a button is
  cosmetic — devtools can write to the DataChannel directly. Gamepad
  input, `/wake`, `/quality`, `/capture-format`, `/reset-dongle` and
  `/restart` are all gated in the server.
- **Cyclic logs go in `/dev/shm`, always.**
- **`scripts/.env` holds real credentials and is never committed.**
  `test_web_stream_auth.c` rewrites it while running, so it saves the
  real one aside and `exit(2)`s if its config path ever resolves to the
  project's own.
- **Run `./tests/run_all.sh` after any change** (215 tests: C, JS,
  native transport, security).
- **The front-end load order is part of the program.** Eleven plain
  scripts sharing one scope, not modules — a declaration hoists within a
  file, not across two. That order exists in three places (`page.html`'s
  tags, `WS_STATIC_FILES[]`, `tests/run_tests.js`) and a test compares
  them, because a file added to one list and not the others passes the
  suite and breaks in a browser.
- **Bump `SETTINGS_VERSION`** (`web/settings.js`) when the *shape* of a
  stored value changes. Adding a key needs no bump; every reader
  defaults.
- **A test that cannot fail is worse than no test.** Both suites were
  verified to actually fail when their bugs were reintroduced.

---

## Measured, not guessed

**Capture format.** `.env.example` ships `mjpeg`; the compiled fallback
when the key is absent is `yuyv`. Both are kept and it is switchable
while running.

| | YUYV | MJPEG |
| --- | --- | --- |
| capture → encoder I420, per frame | **0.40 ms** | 7.81 ms |
| process CPU, two clients | **204%** | 253% |

YUYV is already YUV, so the conversion is a chroma subsample with no
colour-space maths, and SDL takes the card's bytes straight into a
`YUY2` texture. MJPEG's ~50 points of a core buy a fraction of the USB
bytes — which is the only way on a Pi 4, whose USB3 bus is shared with
Ethernet. The MJPEG figures were taken on colour bars, the easiest case
for a JPEG decoder; real content widens the gap.

**Keyframe cadence was the hitch.** `keyframe-max-dist=30` at 60 fps is
two keyframes a second, and a 1080p keyframe steals bitrate from the 59
frames around it. Over 230 s: 57 hitches at 30, **19 at 300**, 27 at
600. All the gain is in leaving 30 behind. `KEYFRAME_MAX_DIST`, default
300. The capture loop itself showed **zero gaps in 230 s** — it was
never the card.

**Headless is not the faster mode.** ~27% CPU headless vs ~170–190%
windowed (the texture upload and present), but browser-side latency is
identical within noise: 32.6 ms headless, 31.6 ms windowed. What changes
is that the local window — a near-zero-latency view of the same
capture — is gone, so the browser's ~32 ms is no longer being compared
against an instant reference next to it.

**The local window used to delay the stream.** `SDL_RenderPresent()`
blocks until the display's next vsync and ran *before* the frame reached
the encoder, so every frame waited on the local monitor before it
started travelling. The push comes first now: the person playing over
the network is the one who cares, and a monitor can be one frame behind.

**Audio, end to end.** PulseAudio capture 22 ms → **~1.2 ms** (local
playback shared the capture thread and its blocking write paced the
loop; a *bigger* buffer made it worse, 34 ms). GStreamer ~0 ms. What
remains is the browser: jitter buffer 31–42 ms and output device 33 ms,
~65 ms largely out of reach. **No continuous clock drift** — NetEq
removed 5469 samples at startup and exactly 0 across four 15 s windows,
disproving the "delay grows over time" theory. Rewriting onto
PulseAudio's async API would be a regression: 3.5 ms with `pa_simple`
against 9.1 ms at best with `PA_STREAM_ADJUST_LATENCY`.

**Input traffic.** 300 reports/s, nearly all identical. With no pacing,
a surplus the console does not drain queues *inside the adapter* — a
50/s gap is twelve seconds of lag after a minute, which no USB reset can
clear. Identical reports are no longer sent (keepalive every 100 ms):
**300 → 10/s idle**, transfer unchanged at 0.85 ms. `GAMEPAD_DEDUP=0`
restores the old behaviour. Real input is untouched — a changed state
goes out on the very next pass. *Hypothesis, not a proven fix.*

**Settings writes.** `saveSettings()` wrote synchronously on every
`oninput`, and the gamepad loop shares that thread, so dragging a slider
added input latency. Coalesced with a flush on `pagehide`: a 500-event
drag went **28.5 ms → 1.3 ms**. Two more of the same kind: the gamepad
debug dump was built 60 times a second while hidden, and
`getGamepadProfile()` rebuilt its defaults every frame — 22 objects per
frame to read four indices. Both now built only when needed; the cached
profile is shared, so `applyGamepadBinding()` copies before writing, or
the first rebind would quietly redefine "default" for that pad.

---

## Root causes worth keeping

- **GCAPI on the wire is 64 bytes**, `[type][length LE16][first][data]`,
  **without** the `0x00` report-id byte GIMX's `s_gppReport` suggests —
  GIMX drops it when sending. 65 bytes shifts everything by one and the
  firmware silently ignores it: USB accepts everything, the console
  reacts to nothing.
- **GCAPI needs a one-time GTuner Pro setup** (Windows or a VM; Wine
  fails on its embedded browser component). Without configuring the Xbox
  360 output profile once, neither passthrough nor injection does
  anything. It is stored on the device and persists.
- **"Select stuck pressed"** was `read_real_controller_state()` reading
  at an unverified offset, combined with a failed read leaving
  `g_real_state` untouched — one bad read latched a button forever. The
  offset is now *measured* (the old one read two bytes short), a failed
  read zeroes the state, and the passthrough merge is live: someone at
  the console plays alongside whoever is on the page.
- **The page read `C2sHelloAck` at the wrong offsets, and it was
  silent.** `audio_rate` sits at byte 14; the page read 16-bit at 18,
  which is `reserved2` — always zero, and zero is exactly how the
  protocol says "this host sends no sound". So the audio decoder was
  never started and the WebSocket path played nothing, with no error
  anywhere. Same family as the GCAPI report-id bug above: a struct read
  by counting, where counting wrong looks like valid data.
  `tests/run_tests.js` now computes the layout from `c2s_protocol.h` and
  fails if the two drift.
- **The browser's settings only reached one of its two encoders.**
  `set_browser_resolution()` wrote `vscale_caps` and
  `set_video_bitrate()` wrote `venc_vp8` — both on the WebRTC chain — so
  on the WebSocket transport the resolution dropdown and the bitrate
  slider did nothing at all. They drive `vscale_web264_caps` and
  `venc_web_h264` as well now, and announce the change to the connected
  pages. One control, one transport running: a slider that moves only
  the encoder nobody is watching is a slider that does nothing.
- **Unchecking vsync on the WebSocket path was a black screen.**
  `setVsync(false)` hands the display to the `<video>` element, which on
  that transport has no source — the decoder draws into the canvas.
  Re-checking it brought the picture back, which is what made it look
  like a vsync bug rather than a surface-ownership one.
- **A replaced WebSocket's handlers still ran, and still owned the
  page.** Closing a socket is asynchronous, so on login -- where the
  token has to travel in a new socket's URL -- the old one's `onclose`
  landed after its replacement was live and set `wsSocket` to null
  (nulling the *new* socket), cleared the *new* ping timer, and
  scheduled a reconnect that opened a third. The second stayed open and
  went on decoding into the same canvas, unreachable. Two painters on
  one canvas is the flicker; three clients on the host for one tab is
  the same bug from the other end; refreshing the page "fixed" it by
  dropping everything at once. Every handler now checks it is still the
  current socket before touching anything.
- **The GMainContext isolation never worked.**
  `g_main_context_push_thread_default()` acquires the context, and the
  gst thread already owns it — GLib said `assertion 'acquired_context'
  failed` on every connection, so `webrtcbin` captured the *global*
  default, exactly what the code meant to avoid. Now built on the gst
  thread through a synchronous invoke, with a timeout.
- **The adapter was killed before it could be handed back.** Clean
  shutdown took 2003 ms; the launcher escalated to SIGKILL at 2000. The
  `LEAVE_CAPTURE` report was routinely never sent, so the adapter stayed
  in capture mode across relaunches. Fixed four ways: the adapter shuts
  down *first*, `libusb_reset_device()` on open, transfer timeout
  1000→250 ms, launcher grace 2→5 s. Now 1004 ms.
- **Waking needs a real gap, not a reset.** `libusb_reset_device()`
  re-enumerates in milliseconds, which was not enough. The reset now
  holds the adapter closed for **3 s** (2 was the shortest unplug ever
  observed to work), sliced so shutdown can interrupt it — a
  three-second block there would recreate the bug above.
- **…and it must fire when the picture returns, not when the script
  ends.** The console is ~10 s from drawing anything. The card is a UVC
  device that generates its own no-signal pattern, so V4L2 reports
  `Status: ok` either way and there is nothing to query — instead the
  frame present when the script finishes becomes the reference, and 1024
  samples across the buffer are compared until enough differ for three
  consecutive frames. Verified on hardware; the plug cycle physically
  drops the adapter off the USB bus and the hot-plug path carries it
  through unattended.
- **The video appsrc queue was unbounded.** appsrc defaults to
  `max-bytes=200000`; a 1080p I420 frame is 3.1 MB. The limit only
  drives signals nothing here listens to, so with `block=false` every
  frame was accepted and the queue grew whenever the encoder fell
  behind — exactly on complex scenes. Now `max-buffers=1
  leaky-type=downstream`. *Not proven to be the freeze:* the one stall
  actually captured showed `recv=0` for a whole second, a producer
  stopping rather than a backlog.
- **Player-gated requests were missing their token.** Two of three call
  sites forgot `X-Player-Token`, so `/quality` was inert and the format
  toggle snapped back — the read-back correctly reported nothing had
  changed, which looked like the switch failing. `playerFetch()` adds it
  now; a test asserts every gated endpoint carries it.
- **`saveSettings()` never updated the in-memory object**, only
  `localStorage` — invisible for settings read once at load, fatal for
  rebind profiles, which re-read on every access.
- **X/Y transposition is the controller, not the console.** Applying it
  everywhere swapped square and triangle on a PS5 DualSense.
  `gamepadNeedsXYSwap()` keys on the pad; the likely root cause is the
  Linux xpad driver's button order. That same Xbox Series X|S pad also
  reports LT/RT as axes 4/5 rather than buttons 6/7.
- **A green stripe down the right of the picture is a width that is not
  a whole number of macroblocks.** H.264 codes in blocks of sixteen;
  854 is 53.4 of them, so 480p was coded at 864 with the ten extra
  columns marked to be ignored, as cropping in the parameter sets.
  Every decoder is meant to honour that. Firefox handed the padding
  over anyway, and padding nobody wrote is chroma at zero, which is
  green. Drawing from `frame.visibleRect` -- which the page already
  did -- is not enough on its own; the fix is to leave nothing to
  honour, so 480p is **848** across now, six pixels and closer to 16:9
  than 864 would be. `bottom_screen_server` hit exactly this and
  reached the same answer (its `bs_encoder.c` rounds every size to
  whole blocks).
  - **1080 is the deliberate exception.** Its height is 67.5 blocks, so
    it is coded at 1088 with eight rows cropped -- what every 1080p
    video in the world does, and the one crop no decoder gets wrong.
    720p needs nothing at all: 1280 and 720 are both whole blocks. That
    is exactly why the stripe appeared at 480p and nowhere else.
- **The tray menu needs the button and the timestamp it is handed.**
  KDE has no XEmbed tray: `xembedsniproxy` republishes the
  `GtkStatusIcon` as a StatusNotifierItem, so a right click arrives as
  a *synthesised* X11 button press with no real event behind it.
  `gtk_menu_popup_at_pointer(menu, NULL)` asks GTK to find that event,
  finds none, and takes the menu's pointer grab with `GDK_CURRENT_TIME`
  -- which loses to the grab plasmashell already holds. The menu drew
  and highlighted but never saw the button release that becomes
  `activate`, so *every* item did nothing; Quit was simply the one
  anybody noticed, Settings being reachable by left-clicking the icon
  instead. `popup-menu` hands over `button` and `activate_time` for
  exactly this, and they were being discarded with a `(void)` cast.
  Use `gtk_menu_popup()` with both, plus
  `gtk_status_icon_position_menu`. Also: `gtk_menu_new()` returns a
  floating reference `gtk_menu_popup()` does not adopt, so a menu built
  per right click and never destroyed leaks one per click.
- **Find instances by walking `/proc`**, not by pid file — a deleted pid
  file used to orphan a running instance, and the second copy failed
  with "device accepts neither YUYV nor MJPEG", which sends you
  inspecting the capture card when the device was simply already open.
- **Hardware comes and goes, and both paths survive it now.** The
  capture card can drop off the bus mid-session (`VIDIOC_QBUF: No such
  device`) — the loop closes the device and waits for it rather than
  exiting and taking the web server with it. A card returning at a
  different resolution is refused: the encoder and every negotiated
  client were built for the original size.

---

## Security

`tests/security/attack.js` fires raw sockets at a running instance
(several probes are malformed in ways an HTTP client would refuse to
send). It never completes a `/wake` with a valid token — that
power-cycles the console's mains supply, and the refusal path is what
matters.

Held up: path traversal in eleven encodings (the router is an
exact-match whitelist, so no filesystem path is ever built from input),
access control with absent/bogus/oversized tokens, header injection,
malformed `Content-Length`, 8 KB paths, 9 KB header lines, 200-header
requests, NUL bytes, method confusion, 60 simultaneous connections.
Buffer sizes are exact and `secure_equals` has no early exit.

**One real weakness, fixed: the login delay did not survive
parallelism.** The 0.5 s penalty blocked only the guessing connection's
own thread — 30 parallel guesses finished in 505 ms instead of 15 s,
~60 attempts a second. Failures are now counted server-wide: five stop
login for everyone for 30 s, and a correct password is refused during a
lockout too (otherwise guessing straight through it would still work).
This lets someone lock the real user out in half-minute bursts, which is
the better half of the trade.

Login flow: `POST /login` returns a 64-hex token from `/dev/urandom`,
held in memory server-side only and in the tab's `sessionStorage` —
deliberately *not* in the persisted settings blob. `may_control` is
decided once, at negotiation time, and baked into the client, so the
input hot path never re-checks it.

### Not fixed, deliberately

- **The `Host` header is not validated**, so DNS rebinding is possible.
  Fixing it means an allowlist that would break access by IP, hostname
  and Tailscale name unless each is listed — a deployment decision, not
  a bug to silently patch.
- **With no `PLAYER_PASSWORD`, `/wake` and `/quality` take any
  request**, including a cross-origin POST from any site the user
  visits. That open mode is deliberate; the answer is to set a password.
- **HTTP is unencrypted.** Fine over Tailscale, not on a plain LAN.

---

## Still open

1. **Control from the app itself** — drive the virtual pad from the
   local SDL/GTK window without going through a browser. Read a local
   pad (SDL_GameController?) and call `gamepad_bridge_update()`
   directly, toggleable from the GTK menu *in addition to* the browser
   path.
2. **STUN for the WebRTC path.** `web/webrtc.js` creates
   `RTCPeerConnection({ iceServers: [] })` and `webrtcbin` gets none
   either, so it works only on the LAN. The WebSocket transport is the
   way around this today and was built for it — but STUN stays worth
   doing for the lower-latency option. The network here is normal NAT,
   not CGNAT, so STUN plus a forwarded UDP range should be enough, no
   TURN. **A first attempt was reverted**: STUN + `min-rtp-port`/
   `max-rtp-port` together got stuck on "connecting" even on the LAN,
   though each piece was verified working in isolation on a bare
   `webrtcbin`. Likely an interaction with multi-client handling,
   `bundle-policy`, or the context push/pop. Retry **one piece at a
   time**, watching `ice-gathering-state`.
3. **Mouse-driven stick feel.** `MOUSE_DECAY = 0.85`
   (`web/keyboard.js`) is a first guess, never tuned with a controller
   in hand. Open question whether identical repeated stick sequences
   from *keyboard* input look bot-like to anti-cheat, and whether
   deliberate variance is worth adding.
4. **Audio sits slightly behind video and input.** Investigate
   `playoutDelayHint` on the audio receiver, the Opus frame size/ptime,
   and whether the capture-side buffer carries more inherent buffering
   than the V4L2 path. Profile before picking a fix.
5. **A SIGSEGV in `pthread_detach`** under a reconnect storm, seen once,
   **not reproduced** — 69 adoptions under ASan produced no report. The
   trigger may have gone with the single-socket guard.
6. **Whether 1080p60 is too much for Firefox** on modest hardware; a
   720p browser stream is the fallback.
7. **The long-session freezes.** Several real defects were fixed along
   the way (the unbounded queue, the keyframe cadence, the report
   pile-up) but none is *proven* to be the cause. What to look for when
   it happens: `video_capture: N ms gap before this frame`. If it
   appears, capture stalled; if the viewer sees a hole and it does not,
   the stall is downstream.
8. **Windows.** Still blocked.

### Two things with no technical fix

- The GitLab and GitHub tokens used during development were pasted in
  clear and **should be revoked**.
- The Android signing key at `~/.android/capture2cloud-signing/` has
  **no backup**. Losing it makes every future update of the app
  impossible.

---

## A trap worth knowing about

The security suite deliberately triggers the 30-second failed-login
lockout. A `/login` right after a run returns 429, so a script that
captures its body as a token sends an empty one — which looks exactly
like a broken endpoint returning 403. That is what a `/reset-dongle`
"failure" turned out to be. The suite waits the lockout out at its own
start; a manual `curl` after a run still has to.
