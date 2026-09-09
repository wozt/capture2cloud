/*
 * capture2cloud -- the stream over a WebSocket, decoded by the browser.
 *
 * The other transport in this page is WebRTC, and it is not going away:
 * its media travels peer-to-peer over UDP, which is what keeps latency
 * honest and what makes a lost packet cost one frame instead of
 * stalling everything behind it.
 *
 * What it cannot do is leave the house. That media never touches the
 * HTTP chain, so a reverse proxy relays the signalling and then watches
 * the stream fail; reaching it from outside needs STUN and possibly a
 * TURN relay. This path exists for that: the SAME protocol the Android
 * and Switch clients speak, carried over a WebSocket, which Cloudflare
 * and Nginx pass without being told anything at all.
 *
 * Which one runs is the host's decision (WEB_TRANSPORT in the .env),
 * read from /shared. Never both: they are two deliveries of two
 * different encodes, and a page that could choose would be a page that
 * disagrees with the host about which encoder is running.
 */

/* The message types, as c2s_protocol.h numbers them. */
var WS_MSG_VIDEO = 1;
var WS_MSG_AUDIO = 2;
var WS_MSG_STREAM_INFO = 21;
var WS_MSG_SHARED = 26;
var WS_MSG_HELLO_ACK = 27;
var WS_MSG_PING = 17;
var WS_MSG_INPUT = 16;
var WS_MSG_KEYFRAME = 22;
/* Header + the 21 pad slots, reused rather than allocated sixty times a
 * second: this is called from the gamepad loop. */
var wsInputBuf = new Uint8Array(8 + 21);

var wsSocket = null;
var wsDecoder = null;
var wsCtx2d = null;
var wsFrames = 0;
var wsKeyframes = 0;
var wsBytes = 0;
var wsLastStatsAt = 0;
var wsLastStatsBytes = 0;
var wsLastStatsFrames = 0;
var wsKbps = 0;
var wsFps = 0;
var wsDropped = 0;
var wsLastKeyRequest = 0;
/* Four frames is about a sixteenth of a second at sixty: deep enough to
 * ride out a hiccup, shallow enough that riding one out never becomes
 * latency worth watching. */
var WS_MAX_DECODE_QUEUE = 4;
/* Set while a deliberate switch is tearing the socket down, so its
 * onclose does not helpfully reconnect the path we are leaving. */
var wsLeaving = false;
var wsPingTimer = null;
/* The reconnect that is already scheduled. Without this the two-second
 * retry and the two-second /shared poll both saw a null socket and both
 * opened one -- which is how one tab became three clients. */
var wsRetryTimer = null;

/* True while the WebSocket path is the one carrying the picture. Read by
 * the control bar, which otherwise reaches for the <video> element -- and
 * on this path that element has no source at all. */
function wsIsActive() { return wsSocket !== null; }

/* One C2S input message: the same 21 bytes, the same header, the same
 * meaning as on the other two clients. */
function wsSendInput(state) {
  if (!wsSocket || wsSocket.readyState !== 1) return;
  wsInputBuf[0] = WS_MSG_INPUT;
  wsInputBuf[1] = 0;
  wsInputBuf[2] = 0; wsInputBuf[3] = 0;
  wsInputBuf[4] = 21; wsInputBuf[5] = 0; wsInputBuf[6] = 0; wsInputBuf[7] = 0;
  wsInputBuf.set(new Uint8Array(state.buffer, state.byteOffset, 21), 8);
  wsSocket.send(wsInputBuf);
}

function wsRequestKeyframe() {
  if (!wsSocket || wsSocket.readyState !== 1) return;
  var h = new Uint8Array(8);
  h[0] = WS_MSG_KEYFRAME;
  wsSocket.send(h);
}

var wsAudioDecoder = null;
var wsAudioChannels = 2;
var wsAudioRate = 48000;
var wsAudioPackets = 0;
/* When the next packet is due, on the context's own clock. */
var wsNextAudioAt = 0;
/*
 * How far ahead of "now" a packet is scheduled.
 *
 * Behind, and the schedule stutters on every packet that arrives
 * fractionally late. Ahead, and the lead quietly becomes latency --
 * which on a stream being played on is the thing being paid for. Forty
 * milliseconds absorbs the jitter of a LAN; past a hundred and fifty the
 * schedule is rebuilt rather than allowed to drift.
 */
var WS_AUDIO_LEAD = 0.04;
var WS_AUDIO_MAX_LEAD = 0.15;

/*
 * The codec string, read out of the stream rather than assumed.
 *
 * WebCodecs takes the profile and level as part of the codec name and it
 * means them: a hardcoded avc1.42E01E pins level 3.0, which decodes
 * nothing at all at 1080p. The three bytes after the SPS NAL header are
 * exactly profile, constraints and level, so they are taken from there.
 */
function wsCodecFromSps(data) {
  for (var i = 0; i + 4 < data.length; i++) {
    var start3 = data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1;
    var start4 = data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 0 && data[i + 3] === 1;
    if (!start3 && !start4) continue;
    var nal = i + (start4 ? 4 : 3);
    if (nal + 3 >= data.length) break;
    if ((data[nal] & 0x1f) !== 7) continue;   /* 7 is the SPS */
    var hex = function (b) { return b.toString(16).padStart(2, '0'); };
    return 'avc1.' + hex(data[nal + 1]) + hex(data[nal + 2]) + hex(data[nal + 3]);
  }
  return null;
}

function wsStartDecoder(codecName) {
  if (wsDecoder) { try { wsDecoder.close(); } catch (e) {} }
  wsDecoder = new VideoDecoder({
    output: function (frame) {
      /*
       * Drawn from the frame's visible rectangle, not the whole frame.
       *
       * A hardware encoder codes in macroblocks of sixteen, so a width
       * that is not a multiple of sixteen is coded larger and the extra
       * columns are marked to be ignored. A decoder that has not
       * cropped hands back the coded frame, and those columns have an
       * untouched chroma plane -- which is green.
       */
      var r = frame.visibleRect;
      if (canvas.width !== (r ? r.width : frame.displayWidth) ||
          canvas.height !== (r ? r.height : frame.displayHeight)) {
        canvas.width = r ? r.width : frame.displayWidth;
        canvas.height = r ? r.height : frame.displayHeight;
      }
      if (r) {
        wsCtx2d.drawImage(frame, r.x, r.y, r.width, r.height, 0, 0, canvas.width, canvas.height);
      } else {
        wsCtx2d.drawImage(frame, 0, 0, canvas.width, canvas.height);
      }
      frame.close();
      /* The gate is over a picture that is now playing. It exists to
       * ask for the click that unlocks SOUND, and that click is still
       * wanted -- but leaving a "start stream" veil over a running
       * stream says the wrong thing. */
      if (gate) gate.classList.add('hidden');
    },
    error: function (e) { log('decoder: ' + e.message); },
  });
  /* No description, which is what tells WebCodecs the stream is Annex B
   * -- and it is: the host repeats the parameter sets in front of every
   * keyframe (h264parse config-interval=-1), so a page can start at any
   * of them rather than only at the first. */
  wsDecoder.configure({ codec: codecName, optimizeForLatency: true });
}

function wsOnVideo(data, keyframe) {
  if (!wsDecoder) {
    /* Nothing until a keyframe: it carries the parameter sets the
     * decoder is configured from, and a correction to a picture that was
     * never there is noise rather than an image. */
    if (!keyframe) return;
    var codecName = wsCodecFromSps(data);
    if (!codecName) { log('no parameter sets in the keyframe'); return; }
    try {
      wsStartDecoder(codecName);
    } catch (e) {
      log('configure ' + codecName + ': ' + e.message);
      wsDecoder = null;
      return;
    }
  }
  if (wsDecoder.state !== 'configured') return;
  if (!keyframe && wsFrames === 0) return;

  /*
   * A decoder that is behind must not be fed harder.
   *
   * 1080p60 is more than some browsers decode in real time, and
   * decode() queues rather than blocks -- so a page that cannot keep up
   * grows a queue that never drains, which is a picture that stops and
   * memory that does not. Frames are dropped here instead, and a
   * keyframe asked for so the gap costs a moment rather than the five
   * seconds until the next scheduled one.
   */
  if (!keyframe && wsDecoder.decodeQueueSize > WS_MAX_DECODE_QUEUE) {
    wsDropped++;
    var now = Date.now();
    if (now - wsLastKeyRequest > 1000) {
      wsLastKeyRequest = now;
      wsRequestKeyframe();
    }
    return;
  }

  wsFrames++;
  if (keyframe) wsKeyframes++;
  wsBytes += data.length;
  try {
    wsDecoder.decode(new EncodedVideoChunk({
      type: keyframe ? 'key' : 'delta',
      timestamp: wsFrames * 1000,
      data: data,
    }));
  } catch (e) {
    log('decode: ' + e.message);
  }
}

/*
 * One WebSocket binary frame carries exactly one protocol message, in
 * the same layout the TCP framing uses -- so this reader is the same
 * eight-byte header the Android and Switch clients read.
 */
function wsOnMessage(bytes) {
  var v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  var type = bytes[0];
  var flags = bytes[1];
  var size = v.getUint32(4, true);
  if (bytes.length < 8 + size) return;
  var body = bytes.subarray(8, 8 + size);

  if (type === WS_MSG_VIDEO) {
    wsOnVideo(body, (flags & 1) !== 0);
  } else if (type === WS_MSG_AUDIO) {
    wsOnAudio(body);
  } else if (type === WS_MSG_HELLO_ACK) {
    var b = new DataView(body.buffer, body.byteOffset, body.byteLength);
    var granted = body[6] === 1;
    /*
     * The ack carries the sound's shape too: a rate of zero means the
     * host is sending none, and drawing a volume control for that would
     * be a control that does nothing.
     *
     * These offsets are C2sHelloAck's, counted rather than guessed, and
     * getting them wrong is silent: reading the rate four bytes late
     * lands in reserved2, which is zero, which reads exactly like a
     * host sending no sound at all -- so the decoder was simply never
     * started and the page played nothing. The layout is packed:
     *   magic 0..3, version 4, accepted 5, may_control 6, reserved 7,
     *   width 8..9, height 10..11, video_codec 12, audio_codec 13,
     *   audio_rate 14..15, audio_channels 16, reserved2 17..19.
     * tests/run_tests.js pins it.
     */
    wsAudioChannels = body[16] || 2;
    wsAudioRate = b.getUint16(14, true);
    log('connected over the websocket, ' +
        b.getUint16(8, true) + 'x' + b.getUint16(10, true) +
        (granted ? ' -- player' : ' -- viewer'));
    setPlayerUi(granted);
    if (wsAudioRate > 0) wsStartAudio();
  } else if (type === WS_MSG_SHARED) {
    /*
     * The settings this page does not own alone, pushed the moment they
     * change rather than found by the two-second poll.
     *
     * C2sShared, packed: width 0, height 2, fps 4, bitrate 6,
     * video_codec 8, capture_mjpeg 9. No transport field -- which one
     * the host runs is not a property of the stream, and still arrives
     * through /shared.
     *
     * applyShared() is the same one the poll calls, so a slider being
     * held is left alone here exactly as it is there.
     */
    var sv = new DataView(body.buffer, body.byteOffset, body.byteLength);
    applyShared({
      height: sv.getUint16(2, true),
      bitrate_kbps: sv.getUint16(6, true),
      capture: body[9] ? 'mjpeg' : 'yuyv',
    });
  } else if (type === WS_MSG_STREAM_INFO) {
    /* The host saying what it is sending now. The decoder is rebuilt at
     * the next keyframe rather than here: the change takes effect some
     * frames later, and one re-initialised early sees the tail of the
     * old stream. */
    if (wsDecoder) { try { wsDecoder.close(); } catch (e) {} }
    wsDecoder = null;
    wsFrames = 0;
  }
}

/*
 * A context and a gain, whichever path made them.
 *
 * The WebRTC path builds its graph from the <video> element, which has
 * nothing in it here -- but the GAIN is the same gain either way, so
 * applyVolume() keeps working across a switch without knowing which
 * transport is running. Reusing it is not a shortcut: two gains would
 * mean a volume slider that moves one of them.
 */
function wsEnsureAudioGraph() {
  if (!audioCtx) {
    try {
      /*
       * "interactive", not a latencyHint of 0. Zero asks for a
       * 128-frame buffer -- under three milliseconds -- which underruns
       * on any pause of the main thread, and the canvas is drawn on that
       * same thread. The milliseconds it saves are spent on glitches.
       */
      audioCtx = new (window.AudioContext || window.webkitAudioContext)(
        { sampleRate: 48000, latencyHint: 'interactive' });
    } catch (e) {
      log('audio: ' + e);
      return false;
    }
  }
  if (!gainNode) {
    gainNode = audioCtx.createGain();
    gainNode.connect(audioCtx.destination);
  }
  applyVolume();
  return true;
}

function wsStartAudio() {
  if (wsAudioDecoder || !('AudioDecoder' in window)) return;
  if (!wsEnsureAudioGraph()) return;

  wsNextAudioAt = 0;
  wsAudioDecoder = new AudioDecoder({
    output: function (data) {
      var channels = data.numberOfChannels;
      var buffer = audioCtx.createBuffer(channels, data.numberOfFrames, 48000);
      for (var c = 0; c < channels; c++) {
        var plane = new Float32Array(data.numberOfFrames);
        data.copyTo(plane, { planeIndex: c, format: 'f32-planar' });
        buffer.copyToChannel(plane, c);
      }
      data.close();

      var src = audioCtx.createBufferSource();
      src.buffer = buffer;
      src.connect(gainNode);
      var now = audioCtx.currentTime;
      if (wsNextAudioAt < now + 0.02 || wsNextAudioAt > now + WS_AUDIO_MAX_LEAD) {
        wsNextAudioAt = now + WS_AUDIO_LEAD;
      }
      src.start(wsNextAudioAt);
      wsNextAudioAt += buffer.duration;
    },
    error: function (e) { log('audio: ' + e.message); },
  });
  /* No description: the host sends bare Opus packets rather than the Ogg
   * encapsulation, which is what WebCodecs takes when none is given. */
  wsAudioDecoder.configure({
    codec: 'opus',
    sampleRate: 48000,
    numberOfChannels: wsAudioChannels || 2,
  });
}

function wsOnAudio(data) {
  if (!wsAudioDecoder || wsAudioDecoder.state !== 'configured') return;
  /* A browser makes no sound until the page has been touched, so the
   * context is nudged rather than left quietly suspended. */
  if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
  wsAudioPackets++;
  try {
    wsAudioDecoder.decode(new EncodedAudioChunk({
      type: 'key',            /* every Opus packet stands alone */
      timestamp: wsAudioPackets * 20000,
      data: data,
    }));
  } catch (e) {
    log('audio decode: ' + e.message);
  }
}

function startWsStream() {
  /* One at a time, always: a second socket is a second decoder drawing
   * into the same canvas, and a second client on the host's count. */
  if (wsSocket) return;
  if (wsRetryTimer) { clearTimeout(wsRetryTimer); wsRetryTimer = null; }
  if (!('VideoDecoder' in window)) {
    log('this browser has no WebCodecs, so it cannot decode this stream');
    return;
  }
  /* The canvas is the surface either way: the vsync path already draws
   * the WebRTC picture into it, so there is one thing to size and one
   * thing to fit. */
  video.style.display = 'none';
  canvas.style.display = 'block';
  /*
   * And stop the vsync path, which draws the <video> element into this
   * same canvas on its own schedule. Two drawers on one canvas is what
   * the flicker between an old frame and a new one was: each was
   * painting over the other, one of them from a video element with
   * nothing in it.
   */
  stopFrames();
  wsCtx2d = canvas.getContext('2d', { alpha: false, desynchronized: true });

  /*
   * The token goes in the query, which is the only place it can go: the
   * browser's WebSocket constructor takes no headers, so X-Player-Token
   * -- how every other request here identifies itself -- is not
   * available. Without it the host can only see a viewer, which is why
   * the pad did nothing: input is refused server-side, exactly as it is
   * for a viewer on the other transport.
   */
  var url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  if (playerToken) {
    url += '?token=' + encodeURIComponent(playerToken);
  }
  /*
   * Held locally as well, so every handler below can ask whether it is
   * still the socket the page is actually using.
   *
   * Closing a WebSocket is asynchronous: the old one's onclose arrives
   * well after its replacement is live. These handlers all reach for
   * module state -- wsSocket, the ping timer, the canvas -- and the old
   * one was happily doing so from beyond the grave. Logging in is where
   * it showed: stopWsStream() then startWsStream(), and moments later
   * the dead socket's onclose set wsSocket to null (nulling the NEW
   * socket), cleared the NEW ping timer, and scheduled a reconnect that
   * opened a third. The second stayed open and went on decoding into
   * the same canvas, unreachable, because the page had thrown away its
   * only reference to it.
   *
   * Two decoders painting one canvas is the flicker between an old
   * frame and a new one; the host counting three clients for one tab is
   * that same bug seen from the other end. A page refresh "fixed" it
   * because it dropped every socket at once.
   */
  var sock = new WebSocket(url);
  wsSocket = sock;
  sock.binaryType = 'arraybuffer';
  sock.onopen = function () {
    /* Opened after being replaced: nothing here wants it, and leaving
     * it open is one more client on the host's count. */
    if (wsSocket !== sock) { try { sock.close(); } catch (e) {} return; }
    log('websocket open, waiting for a keyframe...');
    /*
     * A ping every two seconds, because a page that is only watching
     * says nothing at all -- and the host drops a client that has been
     * silent for ten. Without this a viewer is disconnected and
     * reconnected for as long as they watch.
     */
    if (wsPingTimer) clearInterval(wsPingTimer);
    wsPingTimer = setInterval(function () {
      if (wsSocket !== sock || sock.readyState !== 1) return;
      var h = new Uint8Array(8);
      h[0] = WS_MSG_PING;
      sock.send(h);
    }, 2000);
  };
  sock.onclose = function () {
    /* A socket already replaced: its successor owns the state now, and
     * touching any of it here is what this guard exists to stop. */
    if (wsSocket !== sock) return;
    if (wsPingTimer) { clearInterval(wsPingTimer); wsPingTimer = null; }
    wsSocket = null;
    if (wsLeaving) return;
    log('websocket closed, reconnecting...');
    if (wsRetryTimer) clearTimeout(wsRetryTimer);
    wsRetryTimer = setTimeout(function () {
      wsRetryTimer = null;
      if (!wsLeaving) startWsStream();
    }, 2000);
  };
  sock.onerror = function () { if (wsSocket === sock) log('websocket failed'); };
  sock.onmessage = function (ev) {
    /* The one that matters for the picture: a replaced socket must not
     * decode into the canvas the live one is drawing on. */
    if (wsSocket !== sock) return;
    wsOnMessage(new Uint8Array(ev.data));
  };
}

/* Puts the WebSocket path away: called when the host has been switched
 * to WebRTC and this page is following it. */
function stopWsStream() {
  wsLeaving = true;
  if (wsRetryTimer) { clearTimeout(wsRetryTimer); wsRetryTimer = null; }
  if (wsPingTimer) { clearInterval(wsPingTimer); wsPingTimer = null; }
  if (wsAudioDecoder) { try { wsAudioDecoder.close(); } catch (e) {} }
  wsAudioDecoder = null;
  wsAudioPackets = 0;
  if (wsSocket) { try { wsSocket.close(); } catch (e) {} }
  wsSocket = null;
  if (wsDecoder) { try { wsDecoder.close(); } catch (e) {} }
  wsDecoder = null;
  wsFrames = 0;
  /* Hand the surface back to whichever one the vsync setting asks for,
   * rather than to the video element unconditionally: with vsync on,
   * that left the canvas hidden and its draw loop stopped, so the
   * WebRTC picture came back to an element nobody was showing. */
  setVsync(vsyncBox.checked);
}

/*
 * Follows the host onto the transport it is actually running.
 *
 * Called from the /shared poll, so a switch made on one page moves every
 * other page within a couple of seconds -- and a page that was loaded
 * before the switch is not left waiting on an encoder that stopped.
 */
function applyTransport(which) {
  var wantWs = (which === 'ws');
  if (transportSelect && transportSelect.value !== which) {
    transportSelect.value = which;
  }
  if (wantWs && !wsSocket && !wsRetryTimer) {
    if (pc) { try { pc.close(); } catch (e) {} pc = null; }
    wsLeaving = false;
    startWsStream();
  } else if (!wantWs && wsSocket) {
    stopWsStream();
    retry();
  }
}

/*
 * A browser makes no sound until the page has been touched, and this is
 * a page you are going to touch anyway. Registered once, for every
 * transport: resuming a context that is already running costs nothing.
 */
['pointerdown', 'keydown'].forEach(function (ev) {
  window.addEventListener(ev, function () {
    if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
  });
});

/*
 * What this transport is doing, in the same words the other one uses.
 *
 * The stats line reads the peer connection, and on this path there is
 * none -- so it kept showing the last thing WebRTC had said, which was
 * "VP8" over a stream that had been H.264 for several minutes. A line
 * that is stale is worse than one that is blank: it is believed.
 */
function wsStatsLine() {
  var now = Date.now();
  if (wsLastStatsAt && now > wsLastStatsAt) {
    var dt = (now - wsLastStatsAt) / 1000;
    wsKbps = Math.round(((wsBytes - wsLastStatsBytes) * 8) / 1000 / dt);
    wsFps = Math.round((wsFrames - wsLastStatsFrames) / dt);
  }
  wsLastStatsAt = now;
  wsLastStatsBytes = wsBytes;
  wsLastStatsFrames = wsFrames;

  var size = canvas.width + 'x' + canvas.height;
  return 'h264(ws) ' + size + ' ' + wsFps + 'fps ' + wsKbps + 'kbps' +
         (wsDropped ? '  ' + wsDropped + ' dropped' : '');
}
