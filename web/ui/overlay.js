/*
 * capture2cloud -- what is drawn over the picture
 *
 * The collapsible stats row, and the numbers that fill it: codec,
 * resolution, bitrate and frame rate read from the peer connection,
 * plus how many clients the host says are connected.
 */

/* --- stats row: collapsible, so it doesn't take up too much space --- */
function applyStatsVisibility(visible) {
  infoEl.classList.toggle('hidden', !visible);
  gpDebugEl.classList.toggle('hidden', !visible);
  statsToggle.textContent = 'stats ' + (visible ? '▾' : '▸');
}
statsToggle.onclick = function () {
  var visible = infoEl.classList.contains('hidden');
  applyStatsVisibility(visible);
  saveSettings({ statsVisible: visible });
};
applyStatsVisibility(settings.statsVisible !== false);

/* Menu bar: only visible when the mouse gets near the top of the window
 * (top 25%), not just on hovering the whole page. */
document.addEventListener('mousemove', function (e) {
  if (e.clientY < window.innerHeight * 0.25) {
    bar.classList.add('visible');
  } else {
    bar.classList.remove('visible');
  }
});


/* --- live stats: codec, resolution, bitrate, fps --- */
var lastBytes = 0;
var lastStatsTime = 0;
var lastAudioBytes = 0;
var lastAudioPackets = 0;
function updateStats() {
  if (!pc) return;
  pc.getStats(null)
    .then(function (stats) {
      var videoLine = '';
      var audioLine = '';
      stats.forEach(function (report) {
        if (report.type !== 'inbound-rtp') return;
        if (report.kind === 'video') {
          var now = report.timestamp;
          var bytes = report.bytesReceived || 0;
          var kbps = 0;
          if (lastStatsTime && now > lastStatsTime) {
            kbps = Math.round(((bytes - lastBytes) * 8) / (now - lastStatsTime));
          }
          lastBytes = bytes;
          lastStatsTime = now;
          var fps = report.framesPerSecond || 0;
          var codec = '?';
          if (report.codecId && stats.get(report.codecId)) {
            var mime = stats.get(report.codecId).mimeType || '';
            codec = mime.replace('video/', '');
          }
          videoLine = codec + ' ' + video.videoWidth + 'x' + video.videoHeight + ' ' + fps + 'fps ' + kbps + 'kbps';
        } else if (report.kind === 'audio') {
          /* Diagnostic: distinguishes "Chrome isn't even receiving audio
           * packets" (network/negotiation issue, packets at 0) from
           * "Chrome receives them but plays no sound" (Web Audio /
           * playback issue, packets rising normally). */
          var apackets = report.packetsReceived || 0;
          var abytes = report.bytesReceived || 0;
          var akbps = 0;
          if (lastStatsTime && report.timestamp > lastStatsTime) {
            akbps = Math.round(((abytes - lastAudioBytes) * 8) / (report.timestamp - lastStatsTime));
          }
          var pps = apackets - lastAudioPackets;
          lastAudioBytes = abytes;
          lastAudioPackets = apackets;
          var ctxState = audioCtx ? audioCtx.state : 'not created';
          audioLine = 'audio ' + pps + 'pps ' + akbps + 'kbps ctx:' + ctxState;
        }
      });
      var gamepadLine = 'gamepad: ' + currentGamepadStatus;
      infoEl.textContent =
        videoLine + '  ' + audioLine + '  viewers ' + connectedClients + '  ' + gamepadLine +
        '  v' + C2C_VERSION;
    })
    .catch(function () {});
}
setInterval(updateStats, 1000);

/* How many clients the server currently has connected, as "n/max"
 * (see GET /clients). Polled rather than pushed: it changes rarely and
 * the stats line already refreshes on a timer, so a tiny request every
 * few seconds is simpler than plumbing it through the DataChannel.
 * Slower than the stats tick on purpose -- this is ambient information,
 * not something worth a request per second. */
var connectedClients = '?';
function updateClientCount() {
  fetch('/clients')
    .then(function (r) {
      return r.ok ? r.text() : null;
    })
    .then(function (text) {
      if (text) connectedClients = text;
    })
    .catch(function () {
      connectedClients = '?';
    });
}
updateClientCount();
setInterval(updateClientCount, 3000);
