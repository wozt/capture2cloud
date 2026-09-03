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

Changing any of these changes what every connected client receives, on
both transports at once (the browser's WebRTC stream and the native
binary one are two deliveries of the *same* encode).

| Setting | Lives in | Browser | Android | Switch | GTK |
|---|---|---|---|---|---|
| **Video codec** (H.264 / VP8) | encoder | — | `codec` | `net_send_codec` | — |
| **Stream height** (1080 / 720 / 480) | encoder | `/resolution` | `height` | profile | `browser_height` |
| **Stream width** | derived from height | — | — | — | — |
| **Frame rate** | encoder | — | `fps` | profile | — |
| **Bitrate** (and *automatic*, which sets it) | encoder | `/quality` | `bitrateMbps` | profile | `bitrate_mbps` |
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
| Host, port, transport path | — | ✓ | ✓ | — |
| Saved password, session token | ✓ | ✓ | ✓ | — |
| Vsync | ✓ | — | — | `vsync` |
| Local playback sink | — | — | — | `local_direct_sink` |
| Which local controller drives the console | — | — | — | `gamepad_enabled`, `gamepad_index` |

The stick shaping deserves a word: it is per-client even though it feeds
one adapter, because it shapes *that client's own stick* before it is
sent. Two players on two clients each shape their own.

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

**Browsers** poll `GET /shared` every two seconds, because a page holds
no socket to the host but the WebRTC one, and putting settings through
that would tie them to a stream that may not be up yet. Two seconds is
far below the rate at which anyone changes a setting by hand. The page
no longer pushes its stored bitrate on load, nor its stored capture
format on becoming a player.

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
