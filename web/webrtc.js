/*
 * capture2cloud -- the connection itself
 *
 * Offer, answer, and the retry that keeps trying. Loaded last, and
 * deliberately: it is what starts the stream, and everything it will
 * hand frames and input to has to exist by then.
 */

/* --- WebRTC connection --- */
async function start() {
  pc = new RTCPeerConnection({ iceServers: [] });
  /* Created before the offer: that's what includes it in SDP negotiation. */
  gamepadChannel = pc.createDataChannel('gamepad', { ordered: false, maxRetransmits: 0 });
  pc.addTransceiver('video', { direction: 'recvonly' });
  pc.addTransceiver('audio', { direction: 'recvonly' });
  var stream = new MediaStream();
  video.srcObject = stream;
  pc.ontrack = function (event) {
    stream.addTrack(event.track);
    tryAutoplay();
  };
  pc.onconnectionstatechange = function () {
    log('connection: ' + pc.connectionState);
  };
  var offer = await pc.createOffer();
  await pc.setLocalDescription(offer);
  await new Promise(function (resolve) {
    if (pc.iceGatheringState === 'complete') {
      resolve();
      return;
    }
    function check() {
      if (pc.iceGatheringState === 'complete') {
        pc.removeEventListener('icegatheringstatechange', check);
        resolve();
      }
    }
    pc.addEventListener('icegatheringstatechange', check);
  });
  log('negotiating...');
  /* The session token (if logged in) rides along as a header, so the
   * body stays pure SDP. The server decides viewer-vs-player once, from
   * this, and bakes it into the connection it creates -- which is why
   * logging in requires renegotiating (see doLogin below). */
  var resp = await playerFetch('/offer', { method: 'POST', body: pc.localDescription.sdp });
  if (!resp.ok) {
    log('server error: ' + resp.status);
    return;
  }
  /* The server states, per connection, whether this client may actually
   * control the console. Trusting our own stored token is not enough:
   * tokens live only in the server's memory, so any restart invalidates
   * them while the browser keeps believing it is a player -- the UI
   * would say "player" while every input was silently dropped. */
  var granted = resp.headers.get('X-Player-Granted');
  if (granted !== null) {
    var isPlayer = granted === '1';
    if (!isPlayer && playerToken) {
      /* Stale token: drop it so the login button comes back rather than
       * leaving a dead session in place. */
      playerToken = null;
      try {
        sessionStorage.removeItem('capture2cloud_player_token');
      } catch (e) {}
      log('session expired (server restarted?) -- log in again to play');
    }
    setPlayerUi(isPlayer);
    if (!isPlayer) authRow.classList.remove('hidden');
  }

  var answerSdp = await resp.text();
  await pc.setRemoteDescription({ type: 'answer', sdp: answerSdp });

  /* Ask the browser to hold as little audio/video as it can before
   * playing it.
   *
   * Chrome sizes its jitter buffer for smoothness, not latency: measured
   * here it settled around 42 ms for audio against a 25 ms target, and
   * it grows over time when the sender's clock runs slightly fast --
   * which is what makes the delay creep up the longer a session runs.
   * playoutDelayHint asks for the minimum it can manage; it is a hint,
   * not a guarantee, and the cost of pushing it down is a little less
   * tolerance to network jitter (irrelevant on a LAN, which is what this
   * is for). Unsupported browsers simply ignore the property. */
  try {
    pc.getReceivers().forEach(function (r) {
      if (r.track && (r.track.kind === 'audio' || r.track.kind === 'video')) {
        r.playoutDelayHint = 0;
      }
    });
  } catch (e) {
    log('playoutDelayHint unavailable: ' + e);
  }
}
function retry() {
  start().catch(function (e) {
    log('error: ' + e);
    setTimeout(retry, 2000);
  });
}
retry();
