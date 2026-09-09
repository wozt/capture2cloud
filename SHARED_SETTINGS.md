# Which settings are shared, and which are yours alone

There is **one capture card, one encoder, one adapter**. Everything
downstream of them is per-client. That single sentence decides every row
in the tables below, and it is the rule to apply when a new setting is
added: *does changing this change what the host produces?* If yes it is
shared, it belongs to whoever changed it last, and every other client has
to be told. If no, it is nobody else's business.

This matters because of a specific failure: a second viewer joins, their
saved settings are applied on connection, and the first viewer's picture
silently changes underneath them. A client that arrives **does not get to
decide for the room** — it takes the stream as it is and moves its own
controls to match.

---

## Shared — one value for everyone

**Shared with whom, exactly.** There is more than one encode, and this
line was wrong here before: the browsers' stream and the native binary
stream are produced by **different encoders with their own size, frame
rate and bitrate**. A player changing the resolution from the web page
does not touch what the Android and Switch clients receive, and a
native client asking for 480p30 does not touch the browsers. Verified in
both directions, and pinned by a test.

So "shared" below means *shared by everyone on the same stream*:

| Stream | Who is on it | Where its settings live |
|---|---|---|
| **browser** | every web page | `browser_height`, `bitrate_mbps`; read by `GET /shared`, and pushed as `C2S_MSG_SHARED` to the pages on the WebSocket |
| **native VP8** | the native clients that asked for VP8 | `switch_*[0]`; pushed as `C2S_MSG_SHARED` to that group |
| **native H.264** | the native clients that asked for H.264 | `switch_*[1]`; pushed as `C2S_MSG_SHARED` to that group |

**The browser row is two encoders, and that is deliberate.** WebRTC is
served VP8 and the WebSocket is served H.264, because they are two
deliveries of two different encodes -- but they are **one row** here:
they carry the same height and the same bitrate, and the host runs only
one of them at a time. Driving only one of the two is what made the
resolution dropdown and the bitrate slider do nothing for anybody on
the WebSocket. Keeping them equal also means switching transport does
not silently change the picture.

The capture format is the exception that really is global: there is one
card, and both encoders are fed from it.

**The native side is two streams, not one.** The host runs a VP8 chain
and an H.264 chain, and a client is sent whichever it asked for. There
are still exactly **two encoders and never more** — one per codec, each
shared by everyone watching it; ten clients on H.264 are one H.264
encode.

Each chain has its own size, frame rate and bitrate, and they are
independent: somebody on VP8 dropping to 480p30 does not shrink the
picture of the people on H.264, nor move their menus. And a chain
**nobody is on is not fed at all**, so it encodes nothing: the first
client to ask for a codec starts it, the last one to leave stops it.
Everyone on one codec therefore costs exactly what it did when there
was only one chain.

The codec itself is consequently a **per-client** setting and not a
shared one — the one row that moved out of the table below.

| Setting | Lives in | Browser | Android | Switch | GTK |
|---|---|---|---|---|---|
| **Stream height** (1080 / 720 / 480) | encoder | `/resolution` | `height` | profile | `browser_height` |
| **Stream width** | derived from height | — | — | — | — |
| **Frame rate** | encoder | — | `fps` | profile | — |
| **Bitrate** (and *automatic*, which sets it) | encoder | `/quality` | `bitrateMbps` | profile | `bitrate_mbps` |
| **Transport** (WebRTC / WebSocket) | the host: it runs one | `/transport` | — | — | — |
| **Capture format** (MJPEG / raw YUYV) | capture card | `/capture-format` | — | — | `capture_mjpeg` |
| **Adapter output protocol** (xb360 / switch / ps…) | the adapter's own memory | — | — | — | `output_protocol` |
| **Stream enabled**, **web port**, **native port** | the servers | — | — | — | `stream_enabled`, `web_port`, `switch_port` |

Three notes on that table.

- **Bitrate is the awkward one.** Every client runs its own automatic
  bitrate loop, so three clients steering one encoder means the last one
  to speak wins, repeatedly. The rule: automatic mode is a *request*, and
  a client that is not the one who last set the rate should follow rather
  than fight — its slider moves, its "automatic" box stays ticked, and it
  simply stops pushing while it agrees with what it is getting.
- **The adapter protocol is admin-only and stays GTK-only.** It is listed
  here because it is shared, not because clients should be able to set
  it.
- **Ports and stream-enabled** are shared in the strongest sense — they
  take the stream away — and are deliberately not exposed to clients at
  all.
- **The transport is shared because there is one host.** It runs WebRTC
  or the WebSocket, never both, so a page cannot hold an opinion of its
  own: the others would be left waiting on an encoder that had stopped.
  `WEB_TRANSPORT` in the `.env` only picks what it starts on. Native
  clients are unaffected — they have their own port and their own
  protocol, and never went through either of these.

## Not shared — per client, per device

Nothing here touches the host. Two people can disagree about all of it.

| Setting | Browser | Android | Switch | GTK |
|---|---|---|---|---|
| Volume, mute | ✓ | ✓ | ✓ | `local_volume`, `local_muted` |
| Brightness, contrast, saturation, hue | ✓ | ✓ | ✓ | `brightness`, `contrast` |
| Fullscreen / immersive | ✓ | ✓ | — | — |
| Virtual pad: shown, opacity, colour, labels, d-pad style, layout and moved positions | ✓ | ✓ | ✓ | — |
| Stick deadzone, range, diagonals, trigger thresholds, invert RY | ✓ | ✓ | ✓ | ✓ |
| Diagnostics overlay, stats line | ✓ | ✓ | ✓ | — |
| Menu layout (columns / accordion) | — | ✓ | — | — |
| Which group of the control bar is open | ✓ | — | — | — |
| Host, port, transport path | — | ✓ | ✓ | — |
| Session token (never the password — see below) | ✓ | ✓ | ✓ | — |
| Vsync | ✓ | — | — | `vsync` |
| **Video codec** (H.264 / VP8) — picks which native stream to be on | — | ✓ | ✓ | — |
| Local playback sink | — | — | — | `local_direct_sink` |
| Which local controller drives the console | — | — | — | `gamepad_enabled`, `gamepad_index` |

The stick shaping deserves a word: it is per-client even though it feeds
one adapter, because it shapes *that client's own stick* before it is
sent. Two players on two clients each shape their own.

**Everything on the page is remembered except one thing.** Every
control writes its value to `localStorage`, including the two that look
like window state rather than preference: whether the control bar's
group was left open, and fullscreen — the latter recorded from the
document rather than from the checkbox, since leaving with Escape never
goes through it, and restored at the first user gesture because a
browser refuses `requestFullscreen()` outside one.

The exception is the **password**. It is typed, exchanged for a session
token, and forgotten. The token lives in `sessionStorage` so it dies
with the tab, deliberately not in the settings blob that outlives it,
and the host only keeps it in memory. A test asserts the password
reaches `/login` and appears in neither.

The resolution, the capture format and the transport are written down
too, but are **never re-applied on load** — they belong to the host, and
a page arriving with its own saved values would change the picture for
whoever was already watching. They are a record of what this browser
last chose, nothing more.

## Actions, not settings

`/wake`, `/reset-dongle`, `/restart` and HOME affect everyone but hold no
state, so there is nothing to keep in sync — they are listed only so they
are not mistaken for shared settings. All are players-only, enforced on
the server.

---

## What "keeping in sync" has to mean

1. **On connecting**, a client is *told* the current shared values and
   adopts them. It does not send its own saved shared settings. Its
   controls move to what the room is actually doing.
2. **On change**, whoever changed a shared setting has it applied, and
   every other connected client is told the new value and moves its
   controls — the slider, the dropdown, the tick box — without
   re-sending it back.
3. **Local settings are never sent anywhere**, on either transport.
4. The push must not become a loop: a client applying a value it was
   *told* must not report that value back as a change it made.

## How it is done

**Native clients** (Android, Switch) are *pushed* `C2S_MSG_SHARED`: once
the moment the handshake finishes, and again on every change. It carries
width, height, frame rate, bitrate, codec and capture format.
`C2S_MSG_STREAM_INFO` is left alone and keeps its one meaning —
"re-initialise your decoder now" — because a message that both moves
sliders and rebuilds decoders would rebuild a decoder every time
somebody nudged a slider.

Neither client sends anything on connecting any more. Both used to push
their saved profile and codec the instant they were accepted, which is
the failure at the top of this file: starting a client changed the
stream for everyone already watching.

**Browsers** poll `GET /shared` every two seconds. A page on WebRTC
holds no socket to the host but the peer connection itself, and putting
settings through that would tie them to a stream that may not be up
yet; two seconds is far below the rate at which anyone changes a
setting by hand. The poll is also how *any* page learns the transport
has been switched, which is the one thing that cannot arrive over a
socket that is about to stop.

A page on the **WebSocket** does hold a socket, so it is *also* pushed
`C2S_MSG_SHARED` the moment something changes, exactly as the Android
and Switch clients are — the same message, the same struct, and the
same `applyShared()` the poll feeds, so a slider being held is left
alone on either path. It moves the menus at once instead of within two
seconds.

The page no longer pushes its stored bitrate on load, nor its stored
capture format on becoming a player.

**The GTK window** reconciles once a second against the encoder and the
capture device themselves — not against what it last asked for — so a
format the driver refused shows as refused, and a resolution a player
changed from a phone appears in the window.

Two rules hold everywhere. A value that was *received* is written
straight to the control and never through the control's own change
handler, which would post it back and let two clients correct each other
for ever. And a control touched by hand in the last few seconds is left
alone, so a reply already in flight cannot undo a slider someone is
still holding.
