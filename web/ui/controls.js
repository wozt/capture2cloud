/*
 * capture2cloud -- the control bar
 *
 * Sound, the canvas draw path, resolution, capture format, the shared
 * settings poll, fullscreen, bitrate, the picture adjustments, and the
 * two buttons that reach into the host (wake and restart).
 *
 * What these have in common is that each one is a control the person
 * watching can move; what happens after they move it is somebody
 * else's file.
 */

/* --- sound: one slider from 0 to 100, mute, persisted per browser.
 *
 * The slider reads 0-100%, and 100% is the loudest this can go, which is
 * eight times the stream's own level. It used to be labelled 0-400%,
 * which is accurate and useless: nobody thinks of volume as a number
 * above a hundred, and the top three quarters of the scale all read as
 * "louder than everything else on this machine" anyway.
 *
 * <video>.volume is capped at 1.0 by browsers, so anything above the
 * stream's own level has to go through the Web Audio API (a GainNode,
 * whose gain may exceed 1.0). That graph is created only when it is
 * actually needed: createMediaElementSource() PERMANENTLY diverts the
 * element's audio into it, and on Chromium (unlike Firefox) that
 * diversion was observed to silence the page outright, even with a
 * running context and audio arriving normally. */
/* Where the natural level sits on the 0-100 scale: a quarter of the way
 * up, because the top is four times it. */
var VOLUME_MAX_GAIN = 8;
var audioCtx = null;
var gainNode = null;

/* --- optional low-latency audio route ---
 *
 * By default the <video> element plays the audio itself, and Chrome
 * gives that path a "playback"-sized buffer: measured here, 21.3 ms of
 * base latency, exactly the host's PipeWire quantum (1024 frames at
 * 48 kHz). An AudioContext asked for `latencyHint: 0` reports 2.7 ms
 * instead -- 128 frames -- so routing the stream through Web Audio can
 * cut most of that.
 *
 * Off by default, and deliberately so: this project already hit a
 * Chromium bug where feeding Web Audio from the media element killed
 * the sound outright. This route takes the MediaStream directly
 * (createMediaStreamSource), which is a different code path and should
 * be fine -- but "should" is not "verified on your machine", so it is a
 * switch you can turn off rather than a change imposed on everyone. */
/* Single source of truth for mute, because the two routes silence
 * sound in different places: the element's own `muted` flag normally,
 * the gain node when the low-latency graph is carrying it (where the
 * element stays muted permanently). */
var audioMuted = settings.muted === true;

var initialVolume = settings.volume != null ? settings.volume : 25; /* the stream's own level */
volumeSlider.value = initialVolume;
video.volume = 1;

/* Creates the graph (if not already done), tries to (re)start the
 * AudioContext, and returns a promise that resolves to true ONLY if it is
 * really 'running' -- an AudioContext can stay 'suspended' outside of a
 * genuine user gesture. */
function ensureAudioGraph() {
  if (!audioCtx) {
    try {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      var sourceNode = audioCtx.createMediaElementSource(video);
      gainNode = audioCtx.createGain();
      sourceNode.connect(gainNode);
      gainNode.connect(audioCtx.destination);
    } catch (e) {
      log('audio: ' + e);
      return Promise.resolve(false);
    }
  }
  if (audioCtx.state === 'running') {
    return Promise.resolve(true);
  }
  return audioCtx
    .resume()
    .then(function () {
      return audioCtx.state === 'running';
    })
    .catch(function () {
      return false;
    });
}

/* Only engages the Web Audio graph when necessary (volume > 100%).
 * Resolves to true if sound is usable in any case (native or amplified),
 * false only if a requested amplification failed to start (context
 * blocked). */
function maybeEngageAudioGraph() {
  if (volumeGain(Number(volumeSlider.value)) <= 1 && !gainNode) {
    return Promise.resolve(true);
  }
  return ensureAudioGraph();
}

/* Slider position (0-100) to a gain factor, where 1 is the stream's own
 * level. */
function volumeGain(pct) {
  return (pct / 100) * VOLUME_MAX_GAIN;
}

function applyVolume() {
  var v = audioMuted ? 0 : volumeGain(Number(volumeSlider.value));

  video.muted = audioMuted;

  /* The level is set in BOTH places, because which one is actually
   * carrying the sound is not knowable from here.
   *
   * createMediaElementSource() diverts the element's output into the
   * graph, and from then on video.volume controls nothing -- so once the
   * slider has been past the natural level even once, attenuating the
   * element alone leaves the sound playing. But the graph only carries
   * anything while its AudioContext is actually running, and a suspended
   * context (the browser's default until a real user gesture) leaves the
   * element playing natively -- so attenuating the graph alone fails the
   * other way. Both were tried, and each was a way of not being able to
   * turn the sound off.
   *
   * Setting both is correct in either routing and cannot overshoot:
   * whichever path is live applies its own value, and in the impossible
   * case that both were live the result would be quieter, never louder.
   * The element caps at 1, which is all it can do; amplification above
   * that is the graph's job and only exists when the graph is running,
   * which is exactly when it can be delivered. */
  video.volume = Math.min(1, v);
  if (gainNode) {
    gainNode.gain.value = v;
  }
  vv.textContent = Math.round(Number(volumeSlider.value)) + '%';
}
applyVolume();

function applyMuteButtonLabel() {
  muteBtn.textContent = audioMuted ? 'unmute' : 'mute';
}

volumeSlider.oninput = function () {
  var pct = Number(volumeSlider.value);
  saveSettings({ volume: pct });
  if (volumeGain(pct) > 1) {
    ensureAudioGraph().then(function (unlocked) {
      applyVolume();
      if (!unlocked) log('amplification blocked (audio not unlocked)');
    });
  } else {
    applyVolume();
  }
};

muteBtn.onclick = function () {
  audioMuted = !audioMuted;
  applyMuteButtonLabel();
  applyVolume();
  saveSettings({ muted: audioMuted });
  if (!audioMuted) {
    /* This click is a genuine user gesture: if the graph needs to be
     * engaged (amplification already requested), the AudioContext should
     * actually start here, on every browser. */
    maybeEngageAudioGraph().then(function (unlocked) {
      if (unlocked) gate.classList.add('hidden');
    });
    video.play().catch(function (e) {
      log('playback blocked: ' + e);
    });
  }
};

gateBtn.onclick = function () {
  audioMuted = false;
  applyMuteButtonLabel();
  applyVolume();
  saveSettings({ muted: false });
  video
    .play()
    .then(function () {
      return maybeEngageAudioGraph();
    })
    .then(function (unlocked) {
      if (unlocked) gate.classList.add('hidden');
    })
    .catch(function (e) {
      log('playback blocked: ' + e);
    });
};

/* The video always starts muted (muted autoplay is guaranteed by every
 * browser, so it never freezes at start). If the user had explicitly
 * chosen to mute sound last time, we stay muted without asking anything.
 * Otherwise we try sound directly: if the browser already allows it
 * (permission previously granted to the site), it starts on its own;
 * otherwise the small central button stays visible to unlock it with a
 * click (user gesture required by browsers). */
function tryAutoplay() {
  if (settings.muted === true) {
    audioMuted = true;
    applyVolume();
    applyMuteButtonLabel();
    gate.classList.add('hidden');
    video.play().catch(function () {});
    return;
  }
  audioMuted = false;
  applyVolume();
  video
    .play()
    .then(function () {
      applyMuteButtonLabel();
      return maybeEngageAudioGraph();
    })
    .then(function (unlocked) {
      if (unlocked) {
        gate.classList.add('hidden');
      }
      /* otherwise: gate stays visible, a real click will be needed to
       * unlock amplification (video.play() already succeeded, native
       * sound already works; only the >100% amplification is pending) */
    })
    .catch(function () {
      audioMuted = true;
      applyVolume();
      applyMuteButtonLabel();
      video.play().catch(function () {});
      /* gate stays visible: a click is needed to unlock sound */
    });
}

/* --- "vsync": draw the video into a canvas, one draw per decoded frame.
 *
 * Driven by requestVideoFrameCallback where available, which fires
 * exactly when a new video frame is ready for presentation. The older
 * requestAnimationFrame version (kept as a fallback for browsers
 * without rVFC) fires on the DISPLAY's rhythm instead, which is not the
 * same thing: it would redraw whatever the element happened to hold at
 * that instant, so a frame could be drawn twice, skipped, or caught
 * mid-update -- judder and torn-looking output even though the numbers
 * said 60 fps.
 *
 * `alpha:false` and `desynchronized:false` matter too: an opaque canvas
 * lets the compositor skip blending, and desynchronized:true (the "low
 * latency" hint) explicitly ALLOWS tearing, so it must stay off here. */
var ctx2d = canvas.getContext('2d', { alpha: false, desynchronized: false });
var frameCbId = null;
var rafId = null;
var haveRvfc = typeof video.requestVideoFrameCallback === 'function';

function drawFrame() {
  if (video.videoWidth) {
    if (canvas.width !== video.videoWidth || canvas.height !== video.videoHeight) {
      canvas.width = video.videoWidth;
      canvas.height = video.videoHeight;
    }
    ctx2d.drawImage(video, 0, 0);
  }
}
function scheduleFrame() {
  if (haveRvfc) {
    frameCbId = video.requestVideoFrameCallback(function () {
      drawFrame();
      scheduleFrame();
    });
  } else {
    rafId = requestAnimationFrame(function () {
      drawFrame();
      scheduleFrame();
    });
  }
}
function stopFrames() {
  if (frameCbId !== null && typeof video.cancelVideoFrameCallback === 'function') {
    video.cancelVideoFrameCallback(frameCbId);
  }
  if (rafId !== null) {
    cancelAnimationFrame(rafId);
  }
  frameCbId = null;
  rafId = null;
}
function setVsync(enabled) {
  vsyncBox.checked = enabled;
  if (enabled) {
    video.style.display = 'none';
    canvas.style.display = 'block';
    if (frameCbId === null && rafId === null) scheduleFrame();
  } else {
    stopFrames();
    canvas.style.display = 'none';
    video.style.display = 'block';
  }
}
/* Like the capture format, this reads back what the encoder is actually
 * producing rather than what was requested. It is global -- one encoder
 * serves every browser client -- so another player changing it is a
 * change this page has to notice. */
function refreshResolution() {
  fetch('/resolution')
    .then(function (r) { return r.text(); })
    .then(function (v) {
      v = v.trim();
      if (v === '1080' || v === '720' || v === '480') resolutionSelect.value = v;
    })
    .catch(function () {});
}
refreshResolution();

resolutionSelect.onchange = function (e) {
  noteSharedTouched();
  var wanted = e.target.value;
  saveSettings({ resolution: wanted });
  playerFetch('/resolution', { method: 'POST', body: wanted })
    .then(function () { setTimeout(refreshResolution, 800); })
    .catch(function (err) { log('resolution: ' + err); });
};

/* Reflects what the device is ACTUALLY opened with, not what was last
 * asked for: the driver can refuse a format and fall back to the other
 * one, and a dropdown showing the request rather than the result would
 * quietly lie. */
function refreshCaptureFormat() {
  fetch('/capture-format')
    .then(function (r) { return r.text(); })
    .then(function (fmt) {
      fmt = fmt.trim();
      if (fmt === 'yuyv' || fmt === 'mjpeg') captureFormatSelect.value = fmt;
    })
    .catch(function () {});
}
refreshCaptureFormat();

/* Deliberately no longer restores the saved format on becoming a player.
 *
 * There is one capture device, so this is a GLOBAL setting, and pushing
 * a stored preference at load meant that opening the page reconfigured
 * the card for everyone already watching -- to whatever this browser
 * happened to have saved. The dropdown follows what the machine is
 * actually on (see pollShared below); changing it by hand is still a
 * deliberate act and still applies to everyone. CAPTURE_FORMAT in the
 * .env remains the place to pin a default, since it belongs to the
 * machine rather than to a browser. */
function restoreCaptureFormat() {}

captureFormatSelect.onchange = function (e) {
  noteSharedTouched();
  var wanted = e.target.value;
  saveSettings({ captureFormat: wanted });
  playerFetch('/capture-format', { method: 'POST', body: wanted })
    .then(function () {
      /* The device is closed and reopened, which takes a moment; read
       * back once it has settled so a refused format shows up. */
      setTimeout(refreshCaptureFormat, 1500);
    })
    .catch(function (err) { log('capture format: ' + err); });
};

/* The settings this page does not own alone.
 *
 * One encoder and one capture card serve everybody, so another player
 * changing the resolution, the bitrate or the capture format changes
 * what THIS page is receiving -- and controls that go on showing the
 * old value are worse than no controls, because they look authoritative.
 *
 * Values are written straight to the elements and never through their
 * onchange handlers, which post to the host: a value posted back after
 * being read is how two open pages start correcting each other twice a
 * second, for ever. */
/* When this page last changed one of these by hand. A reply that was
 * already in flight must not undo a slider the user is still holding,
 * and the capture device takes a second and a half to reopen. */
var sharedTouchedAt = 0;
function noteSharedTouched() { sharedTouchedAt = Date.now(); }

function applyShared(sh) {
  if (Date.now() - sharedTouchedAt < 3000) return;
  if (sh.height === 1080 || sh.height === 720 || sh.height === 480) {
    resolutionSelect.value = String(sh.height);
  }
  if (sh.capture === 'yuyv' || sh.capture === 'mjpeg') {
    captureFormatSelect.value = sh.capture;
  }
  if (typeof sh.bitrate_kbps === 'number' && sh.bitrate_kbps > 0) {
    var mbps = Math.round(sh.bitrate_kbps / 1000);
    if (String(mbps) !== quality.value) {
      quality.value = String(mbps);
      /* Setting .value fires no event, so the label beside it would go
       * on showing the old number. */
      qv.textContent = quality.value + ' Mbps';
    }
  }
}

function pollShared() {
  fetch('/shared')
    .then(function (r) { return r.json(); })
    .then(applyShared)
    .catch(function () {});
}
pollShared();
setInterval(pollShared, 2000);

vsyncBox.onchange = function (e) {
  setVsync(e.target.checked);
  saveSettings({ vsync: e.target.checked });
};
setVsync(settings.vsync === true);

/* --- fullscreen: menu checkbox, and a double-click/double-tap in the
 * middle of the video also toggles it (not persisted -- browsers require
 * a fresh user gesture each load anyway, and drop fullscreen on
 * navigation/reload on their own). Vendor-prefixed fallbacks kept for
 * older WebKit/Firefox/IE-derived engines that might still show up on
 * some Android WebViews. */
var fullscreenBox = document.getElementById('fullscreen-toggle');
function isFullscreen() {
  return !!(
    document.fullscreenElement ||
    document.webkitFullscreenElement ||
    document.mozFullScreenElement ||
    document.msFullscreenElement
  );
}
function requestFullscreenOn(el) {
  var fn = el.requestFullscreen || el.webkitRequestFullscreen || el.mozRequestFullScreen || el.msRequestFullscreen;
  if (!fn) return Promise.reject(new Error('fullscreen not supported'));
  return fn.call(el) || Promise.resolve();
}
function exitFullscreen() {
  var fn =
    document.exitFullscreen || document.webkitExitFullscreen || document.mozCancelFullScreen || document.msExitFullscreen;
  if (!fn) return Promise.reject(new Error('fullscreen not supported'));
  return fn.call(document) || Promise.resolve();
}
function toggleFullscreen() {
  if (isFullscreen()) {
    exitFullscreen().catch(function () {});
  } else {
    requestFullscreenOn(document.documentElement).catch(function (e) {
      log('fullscreen: ' + e);
    });
  }
}
fullscreenBox.onchange = function () {
  toggleFullscreen();
};
['fullscreenchange', 'webkitfullscreenchange', 'mozfullscreenchange', 'MSFullscreenChange'].forEach(function (ev) {
  document.addEventListener(ev, function () {
    fullscreenBox.checked = isFullscreen();
  });
});
/* Whether a point is close enough to a virtual control that a tap there
 * was probably meant for it.
 *
 * Checking the event's target was not enough. It catches a tap ON a
 * button, which was never the problem: the problem is a thumb aiming for
 * one and missing, which lands on the video a few pixels away and used
 * to toggle fullscreen. Guarded by distance from each control instead,
 * and by three times its own size -- generous on purpose, since the cost
 * of guarding too much is a gesture that needs aiming at empty screen,
 * and the cost of guarding too little is the thing being fixed.
 *
 * Measured against where the controls actually are rather than where
 * they were laid out, because they can be dragged. */
function nearVirtualControl(x, y) {
  var pad = document.getElementById('vgp');
  if (!pad || pad.classList.contains('hidden')) return false;
  var controls = pad.querySelectorAll('.vgp-anchor');
  for (var i = 0; i < controls.length; i++) {
    var r = controls[i].getBoundingClientRect();
    if (!r.width) continue;
    var cx = r.left + r.width / 2;
    var cy = r.top + r.height / 2;
    /* Three times the size means one-and-a-half times the half-extent
     * out from the centre, in each direction. */
    if (Math.abs(x - cx) <= r.width * 1.5 && Math.abs(y - cy) <= r.height * 1.5) {
      return true;
    }
  }
  return false;
}

document.addEventListener('dblclick', function (e) {
  /* Only in the actual video area, not on the settings bar or the
   * virtual gamepad overlay. */
  if (e.target !== video && e.target !== canvas && e.target !== document.body) {
    return;
  }
  if (nearVirtualControl(e.clientX, e.clientY)) return;
  toggleFullscreen();
});

/* --- quality (target bitrate) --- */
quality.value = settings.qualityMbps != null ? settings.qualityMbps : 12;
var qualityTimer = null;
function sendQuality() {
  qv.textContent = quality.value + ' Mbps';
  noteSharedTouched();
  saveSettings({ qualityMbps: Number(quality.value) });
  /* The bitrate is global -- one shared encoder feeds every client -- so
   * the server refuses this from a viewer. Don't ask: the slider is
   * hidden for them anyway, and the restored-from-settings call made on
   * every page load would otherwise log a 403 for every viewer. */
  if (!playerControlsEnabled) {
    return;
  }
  clearTimeout(qualityTimer);
  qualityTimer = setTimeout(function () {
    playerFetch('/quality', { method: 'POST', body: String(Number(quality.value) * 1000) }).catch(function (e) {
      log('quality: ' + e);
    });
  }, 150);
}
quality.oninput = sendQuality;
/* The label only. Calling sendQuality() here used to POST the saved
 * bitrate on every page load, so opening the page changed the encoder
 * for everyone already watching -- and did it again on each reload.
 * pollShared corrects the slider to what the encoder is really on. */
qv.textContent = quality.value + ' Mbps';

/* --- advanced video adjustments (brightness/contrast/saturation/hue),
 * applied via the CSS `filter` property directly on <video>/<canvas> --
 * composited by the browser/GPU, no per-frame JS work or extra canvas
 * passes, so this should be effectively free latency-wise (unlike
 * reprocessing frames through a canvas/WebGL pass every frame). Applied
 * to both elements since vsync (see setVsync() above) toggles which one
 * is actually visible -- harmless to set the filter on the hidden one
 * too. */
var vidBrightness = document.getElementById('vid-brightness');
var vidContrast = document.getElementById('vid-contrast');
var vidSaturation = document.getElementById('vid-saturation');
var vidHue = document.getElementById('vid-hue');
var vidBrightnessV = document.getElementById('vid-brightness-v');
var vidContrastV = document.getElementById('vid-contrast-v');
var vidSaturationV = document.getElementById('vid-saturation-v');
var vidHueV = document.getElementById('vid-hue-v');
var vidFilterResetBtn = document.getElementById('vid-filter-reset');

function applyVideoFilter() {
  var filter =
    'brightness(' +
    vidBrightness.value +
    '%) contrast(' +
    vidContrast.value +
    '%) saturate(' +
    vidSaturation.value +
    '%) hue-rotate(' +
    vidHue.value +
    'deg)';
  video.style.filter = filter;
  canvas.style.filter = filter;
  vidBrightnessV.textContent = vidBrightness.value + '%';
  vidContrastV.textContent = vidContrast.value + '%';
  vidSaturationV.textContent = vidSaturation.value + '%';
  vidHueV.textContent = vidHue.value + '°';
}
function saveVideoFilterSettings() {
  saveSettings({
    vidBrightness: Number(vidBrightness.value),
    vidContrast: Number(vidContrast.value),
    vidSaturation: Number(vidSaturation.value),
    vidHue: Number(vidHue.value)
  });
}
vidBrightness.value = settings.vidBrightness != null ? settings.vidBrightness : 100;
vidContrast.value = settings.vidContrast != null ? settings.vidContrast : 100;
vidSaturation.value = settings.vidSaturation != null ? settings.vidSaturation : 100;
vidHue.value = settings.vidHue != null ? settings.vidHue : 0;
applyVideoFilter();
[vidBrightness, vidContrast, vidSaturation, vidHue].forEach(function (el) {
  el.oninput = function () {
    applyVideoFilter();
    saveVideoFilterSettings();
  };
});
vidFilterResetBtn.onclick = function () {
  vidBrightness.value = 100;
  vidContrast.value = 100;
  vidSaturation.value = 100;
  vidHue.value = 0;
  applyVideoFilter();
  saveVideoFilterSettings();
};

/* --- wake the console from sleep, via the server-side /wake endpoint
 * (POST /wake -> handle_wake() in web_stream.c -> scripts/
 * wake_console.sh, power-cycling its smart plug through Home Assistant).
 * Manually triggered only. The endpoint backgrounds the script and
 * replies immediately (204) -- it does not wait for the plug to
 * actually confirm on/off, so this button's own feedback is only "the
 * request was sent", not "the console is awake".
 *
 * Players only: cutting the console's power is at least as disruptive as
 * pressing its buttons, so the server requires the same token here as
 * for gamepad input (403 otherwise) -- hiding the button below is just
 * the visible half of that. */
var wakeConsoleTimer = null;
/* The reset takes a few seconds by design (the adapter is held closed so
 * the console sees it actually go away), so the button says so rather
 * than looking like it did nothing. */
/* --- restarting the server ------------------------------------------
 *
 * Sound has been seen to stop arriving with everything still claiming to
 * work, and restarting the capture program is the one thing that has
 * always brought it back. This does that without a trip to the machine.
 *
 * The page then waits and reloads itself. Two seconds is how long the
 * shutdown takes -- the gamepad adapter has to be handed back before
 * anything else -- but a fixed wait would reload into nothing on a slow
 * start, so after those two seconds it asks the server whether it is
 * back and only reloads once it answers. */
var RESTART_SETTLE_MS = 2000;
var RESTART_POLL_MS = 500;
var RESTART_GIVE_UP_MS = 30000;

function waitForServerThenReload(startedAt) {
  if (Date.now() - startedAt > RESTART_GIVE_UP_MS) {
    restartNote.textContent = 'the server has not come back -- reload by hand';
    restartServerBtn.disabled = false;
    restartServerBtn.textContent = 'restart server';
    return;
  }
  /* no-store, or a cached answer would say the server is up while it is
   * still starting. */
  fetch('/clients', { cache: 'no-store' })
    .then(function (r) {
      if (!r.ok) throw new Error('not ready');
      restartNote.textContent = 'back -- reloading';
      location.reload();
    })
    .catch(function () {
      setTimeout(function () { waitForServerThenReload(startedAt); }, RESTART_POLL_MS);
    });
}

restartServerBtn.onclick = function () {
  restartServerBtn.disabled = true;
  restartServerBtn.textContent = 'restarting...';
  restartNote.textContent = '';
  playerFetch('/restart', { method: 'POST' })
    .then(function (resp) {
      if (!resp.ok) {
        log(resp.status === 403 ? 'restart: log in to play first'
                                : 'restart: server error ' + resp.status);
        restartServerBtn.disabled = false;
        restartServerBtn.textContent = 'restart server';
        return;
      }
      restartNote.textContent = 'waiting for the server...';
      var startedAt = Date.now();
      setTimeout(function () { waitForServerThenReload(startedAt); }, RESTART_SETTLE_MS);
    })
    .catch(function (e) {
      /* The socket dying IS the restart happening: the server answers
       * before it goes, but a request in flight when it does looks like
       * a network error. Waiting is the right response either way. */
      log('restart: ' + e);
      restartNote.textContent = 'waiting for the server...';
      var startedAt = Date.now();
      setTimeout(function () { waitForServerThenReload(startedAt); }, RESTART_SETTLE_MS);
    });
};

resetDongleBtn.onclick = function () {
  resetDongleBtn.disabled = true;
  resetDongleBtn.textContent = 'resetting...';
  playerFetch('/reset-dongle', { method: 'POST' })
    .then(function (resp) {
      if (!resp.ok) {
        log(resp.status === 403 ? 'reset dongle: log in to play first'
                                : 'reset dongle: server error ' + resp.status);
      }
    })
    .catch(function (e) { log('reset dongle: ' + e); })
    .then(function () {
      setTimeout(function () {
        resetDongleBtn.disabled = false;
        resetDongleBtn.textContent = 'reset dongle';
      }, 4000);
    });
};

wakeConsoleBtn.onclick = function () {
  wakeConsoleBtn.disabled = true;
  wakeConsoleBtn.textContent = 'waking...';
  playerFetch('/wake', { method: 'POST' })
    .then(function (resp) {
      wakeConsoleBtn.textContent = resp.ok ? 'sent!' : 'failed';
      if (!resp.ok) log(resp.status === 403 ? 'wake: log in to play first' : 'wake: server error ' + resp.status);
    })
    .catch(function (e) {
      wakeConsoleBtn.textContent = 'failed';
      log('wake: ' + e);
    })
    .finally(function () {
      clearTimeout(wakeConsoleTimer);
      wakeConsoleTimer = setTimeout(function () {
        wakeConsoleBtn.disabled = false;
        wakeConsoleBtn.textContent = 'wake console';
      }, 2000);
    });
};
