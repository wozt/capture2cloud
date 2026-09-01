window.onerror = function (msg, url, line, col, err) {
  var st = document.getElementById('st');
  if (st) st.textContent = 'JS error: ' + msg + ' @' + line;
};

var STORAGE_KEY = 'capture2cloud_settings';

/* Bump SETTINGS_VERSION whenever the SHAPE of a stored value changes in
 * a way old data can't satisfy (a renamed key, a value that used to be a
 * number and is now an object, a profile format change...), and add a
 * matching step to migrateSettings(). Simply ADDING a new key needs no
 * bump: every reader already handles its key being absent (they all use
 * `settings.x != null ? settings.x : <default>`).
 *
 * Without this, a shape change would silently feed old-format data to
 * new code -- and since everything lives in one JSON blob under a single
 * key, the failure mode is a confusing half-broken UI rather than a
 * clean reset. Storage that can't be migrated is discarded rather than
 * guessed at: losing UI preferences is cheap, running on malformed
 * state is not.
 *
 * Version history:
 *   (unversioned) -- everything before profiles existed
 *   1             -- first versioned schema; gamepadProfiles keyed by
 *                    gamepad id, keyboardProfiles keyed by profile name
 *   2             -- a keyboard profile became { keys, mouse } instead
 *                    of a flat field->keycode map, so mouse buttons and
 *                    wheel can be bound alongside keys
 */
var SETTINGS_VERSION = 3;

function migrateSettings(stored) {
  if (!stored || typeof stored !== 'object') return {};
  var version = typeof stored.settingsVersion === 'number' ? stored.settingsVersion : 0;
  if (version === SETTINGS_VERSION) return stored;
  if (version > SETTINGS_VERSION) {
    /* Storage written by a NEWER build of the page (the user opened a
     * newer version elsewhere, then came back to this one). Its shape is
     * unknown to this code, so start clean rather than misread it. */
    return {};
  }

  /* version 0 -> 1: the pre-versioning blob only held scalar UI prefs
   * (volume, quality, vsync, thresholds, overlay layout...) whose
   * meaning is unchanged, so it carries over as-is -- there is nothing
   * to rewrite, only a version stamp to add. Profile keys didn't exist
   * back then; their absence is already the "no profiles yet" case every
   * reader handles. */
  if (version < 1) {
    stored.settingsVersion = 1;
  }

  /* 1 -> 2: keyboard profiles were a flat { field: keycode } map; they
   * now hold a separate mouse map alongside, so wrap the old contents
   * as `keys` and start with no mouse bindings. Existing key bindings
   * survive untouched. */
  if (version < 2 && stored.keyboardProfiles) {
    for (var name in stored.keyboardProfiles) {
      var p = stored.keyboardProfiles[name];
      if (p && !p.keys) {
        stored.keyboardProfiles[name] = { keys: p, mouse: {} };
      }
    }
  }
  /* 2 -> 3: the volume slider ran 0-400 and now runs 0-100 for the same
   * range of loudness, so a stored position means a quarter of what it
   * used to. Left alone, everyone's sound would have jumped to maximum
   * on the next page load. */
  if (version < 3 && typeof stored.volume === 'number') {
    stored.volume = Math.round(Math.min(400, Math.max(0, stored.volume)) / 4);
  }

  stored.settingsVersion = SETTINGS_VERSION;
  return stored;
}

function loadSettings() {
  try {
    return migrateSettings(JSON.parse(localStorage.getItem(STORAGE_KEY))) || {};
  } catch (e) {
    return {};
  }
}
/* Pending changes not yet written to localStorage, and the timer that
 * will write them. */
var settingsDirty = null;
var settingsFlushTimer = null;
var SETTINGS_FLUSH_MS = 200;

/* Does the actual persist: read what is stored (another tab may have
 * written), merge, write back. */
function flushSettings() {
  if (settingsFlushTimer !== null) {
    clearTimeout(settingsFlushTimer);
    settingsFlushTimer = null;
  }
  if (!settingsDirty) return;
  var pending = settingsDirty;
  settingsDirty = null;
  try {
    var s = loadSettings();
    for (var k in pending) s[k] = pending[k];
    s.settingsVersion = SETTINGS_VERSION;
    localStorage.setItem(STORAGE_KEY, JSON.stringify(s));
  } catch (e) {}
}

/* The in-memory update is immediate -- the rebind profiles are re-read
 * from `settings` at runtime and must never see stale data. Only the
 * WRITE is deferred.
 *
 * That write is not cheap: it parses the whole stored blob (profiles,
 * key bindings, overlay button positions), merges, re-serialises it, and
 * localStorage.setItem is synchronous. Ten `oninput` handlers call this
 * -- volume, both trigger thresholds, overlay opacity and colour, the
 * four video filters, quality -- and oninput fires on every pixel of a
 * drag. Doing all of that per event blocked the main thread, and the
 * gamepad loop lives on that same thread: dragging a slider added input
 * latency. Coalescing turns a drag into one write. */
function saveSettings(patch) {
  for (var k in patch) settings[k] = patch[k];
  settings.settingsVersion = SETTINGS_VERSION;

  if (!settingsDirty) settingsDirty = {};
  for (var k2 in patch) settingsDirty[k2] = patch[k2];

  if (settingsFlushTimer === null) {
    settingsFlushTimer = setTimeout(flushSettings, SETTINGS_FLUSH_MS);
  }
}

/* A drag that is still in flight when the tab goes away would otherwise
 * lose its last change. */
window.addEventListener('pagehide', flushSettings);
document.addEventListener('visibilitychange', function () {
  if (document.visibilityState === 'hidden') flushSettings();
});
var settings = loadSettings();

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

var pc = null;

function log(s) {
  statusEl.textContent = s;
}

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

/* --- sound: one slider from 0 to 100, mute, persisted per browser.
 *
 * The slider reads 0-100%, and 100% is the loudest this can go, which is
 * four times the stream's own level. It used to be labelled 0-400%,
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
var VOLUME_MAX_GAIN = 4;
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

/* Restores the saved choice once, on becoming a player.
 *
 * Note this is a GLOBAL setting -- there is one capture device -- so
 * what is stored per browser is "the format I want the machine to be
 * on", and the last player to load the page wins. That is fine with one
 * player; with several, CAPTURE_FORMAT in the .env is the place to pin
 * it, since it applies at startup and belongs to the machine. */
var captureFormatRestored = false;

function restoreCaptureFormat() {
  if (captureFormatRestored || !playerControlsEnabled) return;
  captureFormatRestored = true;
  var saved = settings.captureFormat;
  if (saved !== 'yuyv' && saved !== 'mjpeg') return;
  fetch('/capture-format')
    .then(function (r) { return r.text(); })
    .then(function (active) {
      if (active.trim() === saved) return; /* already there, don't churn the device */
      playerFetch('/capture-format', { method: 'POST', body: saved })
        .then(function () { setTimeout(refreshCaptureFormat, 1500); })
        .catch(function () {});
    })
    .catch(function () {});
}

captureFormatSelect.onchange = function (e) {
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
document.addEventListener('dblclick', function (e) {
  /* Only in the actual video area, not on the settings bar or the
   * virtual gamepad overlay -- two quick taps on a virtual button
   * shouldn't ever be interpreted as "toggle fullscreen". */
  if (e.target === video || e.target === canvas || e.target === document.body) {
    toggleFullscreen();
  }
});

/* --- quality (target bitrate) --- */
quality.value = settings.qualityMbps != null ? settings.qualityMbps : 12;
var qualityTimer = null;
function sendQuality() {
  qv.textContent = quality.value + ' Mbps';
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
sendQuality();

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
    bare: [wakeConsoleBtn, resetDongleBtn, rebindBtn, padTestBtn, vidFilterResetBtn, vgpResetBtn]
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
        videoLine + '  ' + audioLine + '  viewers ' + connectedClients + '  ' + gamepadLine;
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

/* --- gamepad: browser Gamepad API -> DataChannel -> Titan One.
 * Order of the 21 bytes sent = the GAMEPAD_XB360_* entries from
 * gamepad_bridge.h on the server side (signed values, -100..100). The
 * server does no transformation at all: all the mapping happens here.
 * Unreliable/unordered channel (like UDP): this is a current state sent
 * continuously, no point retransmitting a stale value. */
var gamepadChannel = null;
var gamepadState = new Int8Array(21);
var selectedGamepadId = settings.gamepadId || null;
var currentGamepadStatus = 'none';

/* In "Virtual buttons" mode, a real gamepad plugged in and detected by
 * the browser stays active too, combined with the touch overlay -- so
 * touching a button and pressing the real one both work, and the
 * on-screen buttons/sticks visually reflect the real controller's input
 * even without any touch. Touch handlers write to vgpTouchState (never
 * directly to gamepadState); the per-frame combiner in sendGamepadState()
 * adds a freshly-read real-gamepad state on top before sending. */
var vgpTouchState = new Int8Array(21);

/* Sent in place of the real state while input is suppressed. Kept
 * separate rather than zeroing `gamepadState`, because the controller
 * test popup has to keep DISPLAYING what would be sent while nothing
 * actually reaches the console -- zeroing the state would blank the
 * very diagram the popup exists to show. */
var zeroState = new Int8Array(21);

/* True when input must not reach the console: while testing buttons
 * (unless explicitly allowed) -- pressing everything to check it
 * shouldn't also press it in the running game. */
function inputIsSuppressed() {
  return padTestIsOpen() && padTestBlock.checked;
}

function sendGamepadBuffer() {
  if (!gamepadChannel || gamepadChannel.readyState !== 'open') return;
  gamepadChannel.send(inputIsSuppressed() ? zeroState.buffer : gamepadState.buffer);
}

/* "Keyboard/mouse" mode's own state, declared here (rather than down by
 * the rest of its logic) so it already exists by the time the initial
 * mode-activation calls run further below -- kbState/kbHeldKeys are
 * filled in by keydown/keyup handlers, mouseStickX/Y by mousemove while
 * pointer-locked. See the "Keyboard/mouse mode" section for the rest. */
var kbState = new Int8Array(21);
var kbHeldKeys = {};
var mouseStickX = 0,
  mouseStickY = 0;

/* Rebind panel DOM refs + listening state -- declared here (not down by
 * the rest of the rebind panel logic) for the same reason as the
 * keyboard state above: gamepadSelect's init/onchange code below needs
 * them to already exist. */
var rebindEl = document.getElementById('rebind');
var rebindBtn = document.getElementById('rebind-btn');
var rebindTitle = document.getElementById('rebind-title');
var rebindProfileRow = document.getElementById('rebind-profile-row');
var rebindRowsEl = document.getElementById('rebind-rows');
var rebindNote = document.getElementById('rebind-note');
var rebindCloseBtn = document.getElementById('rebind-close');
var rebindListening = null; // { kind: 'gamepad'|'keyboard', field }
var rebindGpSnapshot = null;

/* Up/down inversion of the right stick, on the fly -- observed inverted
 * on some gamepads/console-side translations. */
invertRyBox.checked = settings.invertRy === true;
invertRyBox.onchange = function (e) {
  saveSettings({ invertRy: e.target.checked });
};

/* Adjustable trigger thresholds: on some gamepads (Xbox Series X|S
 * observed here), LT/RT do not come back as standard buttons but as
 * extra AXES (4 and 5), on a range from -1.00 (released) to 1.00 (fully
 * pressed) -- hence a threshold, not a simple boolean. The right value
 * can vary from one gamepad to another, hence the on-the-fly setting
 * rather than a fixed constant. */
ltThresholdSlider.value = settings.ltThreshold != null ? settings.ltThreshold : 0.3;
rtThresholdSlider.value = settings.rtThreshold != null ? settings.rtThreshold : 0.3;
ltv.textContent = Number(ltThresholdSlider.value).toFixed(2);
rtv.textContent = Number(rtThresholdSlider.value).toFixed(2);
ltThresholdSlider.oninput = function () {
  ltv.textContent = Number(ltThresholdSlider.value).toFixed(2);
  saveSettings({ ltThreshold: Number(ltThresholdSlider.value) });
};
rtThresholdSlider.oninput = function () {
  rtv.textContent = Number(rtThresholdSlider.value).toFixed(2);
  saveSettings({ rtThreshold: Number(rtThresholdSlider.value) });
};

/* --- per-stick limits ------------------------------------------------
 *
 * Two numbers per stick, and the outer one is the important half.
 *
 * A stick does not reach its electrical maximum, least of all on a
 * diagonal, so pushing it all the way sends 0.8 rather than 1.0 and the
 * console reads a gentle push. Setting the range to what the stick
 * actually reaches makes the end of its travel mean full deflection
 * again, and the travel in between is stretched across the whole scale
 * so the middle stays proportional rather than jumping.
 *
 * Per stick because the two wear at different rates: the left one takes
 * most of the movement in most games and goes first. */
var stickLimits = {
  left: { deadzone: lStickDeadzone, range: lStickRange, diagonal: lStickDiagonal },
  right: { deadzone: rStickDeadzone, range: rStickRange, diagonal: rStickDiagonal }
};

function bindStickLimit(slider, label, key, fallback) {
  var out = document.getElementById(label);
  slider.value = settings[key] != null ? settings[key] : fallback;
  out.textContent = slider.value + '%';
  slider.oninput = function () {
    out.textContent = slider.value + '%';
    var change = {};
    change[key] = Number(slider.value);
    saveSettings(change);
  };
}
bindStickLimit(lStickDeadzone, 'lstick-deadzone-v', 'lStickDeadzone', 5);
bindStickLimit(lStickRange, 'lstick-range-v', 'lStickRange', 100);
bindStickLimit(rStickDeadzone, 'rstick-deadzone-v', 'rStickDeadzone', 5);
bindStickLimit(rStickRange, 'rstick-range-v', 'rStickRange', 100);
bindStickLimit(lStickDiagonal, 'lstick-diagonal-v', 'lStickDiagonal', 100);
bindStickLimit(rStickDiagonal, 'rstick-diagonal-v', 'rStickDiagonal', 100);

/* Both axes of one stick, from the Gamepad API's -1..1 to the wire's
 * -100..100, with that stick's two limits applied.
 *
 * The limits apply to the LENGTH of the vector, not to each axis on its
 * own, because a stick is round. The hardware limits the two together to
 * a circle, so pushing fully into a corner gives about 0.71 on each axis
 * and never 1.0; judged per axis that reads as three quarters of a push,
 * which is why a diagonal would not reach full deflection even with the
 * range set to 90%. Scaling the vector and keeping its direction means a
 * stick pushed as far as it goes is full deflection wherever it is
 * pointed. */
function stickPairToWire(rawX, rawY, which) {
  var limits = stickLimits[which];
  var dead = Number(limits.deadzone.value) / 100;
  var satAxis = Number(limits.range.value) / 100;
  var satDiag = Number(limits.diagonal.value) / 100;

  var mag = Math.sqrt(rawX * rawX + rawY * rawY);
  if (mag <= dead || mag < 1e-6) return [0, 0];

  var ax = Math.abs(rawX), ay = Math.abs(rawY);
  var peak = Math.max(ax, ay);
  /* 0 straight along an axis, 1 at exactly 45 degrees. The two outer
   * limits blend by this, so there is no seam anywhere in the circle. */
  var diagonality = peak > 1e-6 ? Math.min(ax, ay) / peak : 0;
  var sat = satAxis + (satDiag - satAxis) * diagonality;
  if (sat <= dead) sat = dead + 0.1;

  /* How far along the usable travel, 0 at the deadzone and 1 at the
   * outer limit, so the middle stays proportional rather than jumping. */
  var t = Math.min(1, (mag - dead) / (sat - dead));
  /* Scaled by the LARGER component, not by the length: at the outer
   * limit the dominant axis is exactly 100 whatever the direction, so a
   * corner comes out as 100/100 rather than 71/71 -- which is what "as
   * far as it goes" should mean for something handed to a game. */
  var scale = (t * 100) / peak;
  var clamp = function (v) { return Math.max(-100, Math.min(100, Math.round(v))); };
  return [clamp(rawX * scale), clamp(rawY * scale)];
}

/* One axis, for anything that only has one. Same rule: with the other
 * axis at rest the vector's length is this axis's own value. */
function stickToWire(raw, which) {
  return stickPairToWire(raw, 0, which)[0];
}

function clamp100(v) {
  return Math.max(-100, Math.min(100, Math.round(v * 100)));
}

/* --- virtual on-screen gamepad: touch overlay (e.g. for Android), a
 * fixed extra entry in the same dropdown as real gamepads. Drives
 * `gamepadState` directly from touch input instead of polling
 * navigator.getGamepads() -- Pointer Events are used throughout (not
 * touch events) because each button/stick captures its own pointer via
 * setPointerCapture(): several fingers held on different controls at
 * once (a stick + several buttons) are naturally independent, no shared
 * touch-list bookkeeping needed, and a finger sliding slightly off a
 * button while held stays captured by it instead of leaking onto a
 * neighboring control. */
var VIRTUAL_GAMEPAD_ID = '__virtual__';
var KEYBOARD_GAMEPAD_ID = '__keyboard__';
var vgpEl = document.getElementById('vgp');
var vgpLayoutRow = document.getElementById('vgp-layout-row');
var vgpLayoutSelect = document.getElementById('vgp-layout');
var vgpSymmetricRow = document.getElementById('vgp-symmetric-row');
var vgpSymmetricBox = document.getElementById('vgp-symmetric');
var vgpEditRow = document.getElementById('vgp-edit-row');
var vgpEditBox = document.getElementById('vgp-edit-toggle');
var vgpResetBtn = document.getElementById('vgp-reset');
var vgpColorRow = document.getElementById('vgp-color-row');
var vgpColorInput = document.getElementById('vgp-color');
var vgpOpacityRow = document.getElementById('vgp-opacity-row');
var vgpOpacityInput = document.getElementById('vgp-opacity');
var vgpOpacityV = document.getElementById('vgp-opacity-v');

/* Overall opacity of the whole overlay (buttons, sticks, d-pad, labels --
 * everything), so it can be faded down to see more of the game through
 * it rather than only ever the fixed ~14% backgrounds baked into the
 * CSS. Applied as a plain CSS opacity on the #vgp container: simplest
 * way to fade every element uniformly together in one go. */
function applyVgpOpacity(pct) {
  vgpEl.style.opacity = String(pct / 100);
  vgpOpacityV.textContent = pct + '%';
}
vgpOpacityInput.value = settings.vgpOpacity != null ? settings.vgpOpacity : 100;
applyVgpOpacity(Number(vgpOpacityInput.value));
vgpOpacityInput.oninput = function () {
  saveSettings({ vgpOpacity: Number(vgpOpacityInput.value) });
  applyVgpOpacity(Number(vgpOpacityInput.value));
};

/* Custom accent color for the neutral controls (sticks, d-pad, shoulder/
 * trigger buttons, start/select/guide, L3/R3) -- NOT the face buttons,
 * which keep their brand-skin colors (recoloring those would defeat the
 * point of picking a layout). Applied as CSS variables on #vgp so a
 * single color choice tints background/border/pressed-state/knob
 * consistently everywhere via plain CSS, no per-element rewriting. */
function hexToRgba(hex, alpha) {
  var m = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex || '');
  var r = m ? parseInt(m[1], 16) : 255;
  var g = m ? parseInt(m[2], 16) : 255;
  var b = m ? parseInt(m[3], 16) : 255;
  return 'rgba(' + r + ',' + g + ',' + b + ',' + alpha + ')';
}
function applyVgpColor(hex) {
  vgpEl.style.setProperty('--vgp-btn-bg', hexToRgba(hex, 0.16));
  vgpEl.style.setProperty('--vgp-btn-border', hexToRgba(hex, 0.5));
  vgpEl.style.setProperty('--vgp-btn-bg-pressed', hexToRgba(hex, 0.55));
  vgpEl.style.setProperty('--vgp-stick-bg', hexToRgba(hex, 0.12));
  vgpEl.style.setProperty('--vgp-stick-border', hexToRgba(hex, 0.4));
  vgpEl.style.setProperty('--vgp-knob-bg', hexToRgba(hex, 0.4));
  vgpEl.style.setProperty('--vgp-knob-border', hexToRgba(hex, 0.7));
}
vgpColorInput.value = settings.vgpColor || '#ffffff';
applyVgpColor(vgpColorInput.value);
vgpColorInput.oninput = function () {
  saveSettings({ vgpColor: vgpColorInput.value });
  applyVgpColor(vgpColorInput.value);
};

/* The face buttons are 4 fixed screen positions (top/left/right/bottom).
 * Which GAMEPAD_XB360_* index each position drives is constant across
 * every layout -- the layout picker is a pure skin (label + color), it
 * does not change behavior, so muscle memory built up on one skin (e.g.
 * always tapping "top" for the same in-game action) keeps working when
 * switching skins.
 *
 * These indices are NOT simply GAMEPAD_XB360_Y/X/B/A taken as-is, and
 * they are NOT the same compensation used for the real gamepad's mapping
 * above (that one compensates this specific physical controller's own
 * nonstandard button order as reported by the browser -- the virtual
 * overlay never goes through the browser's Gamepad API at all, so
 * there's nothing of that quirk to compensate for here). This exact
 * combination (Y/X natural, A/B swapped) is the one empirically
 * confirmed correct on-screen by the user testing each pair separately
 * -- Y/X needed the natural GAMEPAD_XB360_Y(17)/X(20) indices, while
 * A/B needed GAMEPAD_XB360_B(18)/A(19) swapped relative to their own
 * natural indices. If this ever breaks again (firmware/adapter change),
 * re-test each of the 4 face buttons individually -- don't assume the
 * two pairs behave the same way, they haven't so far. */
var FACE_SLOTS = { top: 17, left: 20, right: 18, bottom: 19 };
/* Skin only: label + color shown at each (fixed) position. Nintendo uses
 * the classic SNES-derived color set (X=blue, Y=green, A=red, B=yellow)
 * that Nintendo itself still uses in manuals/UI -- distinct on all four
 * buttons, unlike an earlier version of this table that accidentally
 * reused red for two of them. */
var FACE_SKIN_ALPHA = 0.55;
var FACE_SKINS = {
  xbox: {
    top: { label: 'Y', color: hexToRgba('#c99a00', FACE_SKIN_ALPHA) },
    left: { label: 'X', color: hexToRgba('#2f6fd0', FACE_SKIN_ALPHA) },
    right: { label: 'B', color: hexToRgba('#c73a3a', FACE_SKIN_ALPHA) },
    bottom: { label: 'A', color: hexToRgba('#3a9a4a', FACE_SKIN_ALPHA) }
  },
  playstation: {
    top: { label: '△', color: hexToRgba('#3aada3', FACE_SKIN_ALPHA) },
    left: { label: '□', color: hexToRgba('#c774b0', FACE_SKIN_ALPHA) },
    right: { label: '○', color: hexToRgba('#c75a5a', FACE_SKIN_ALPHA) },
    bottom: { label: '✕', color: hexToRgba('#4a7fd0', FACE_SKIN_ALPHA) }
  },
  nintendo: {
    top: { label: 'X', color: hexToRgba('#2f6fd0', FACE_SKIN_ALPHA) },
    left: { label: 'Y', color: hexToRgba('#2f9a5a', FACE_SKIN_ALPHA) },
    right: { label: 'A', color: hexToRgba('#c73a3a', FACE_SKIN_ALPHA) },
    bottom: { label: 'B', color: hexToRgba('#d0b400', FACE_SKIN_ALPHA) }
  }
};
/* Shoulder/trigger/menu/stick-click labels also follow each brand's own
 * real terms where one exists. Xbox has no official "L3/R3" branding
 * (that's Sony's) -- Microsoft's own docs call these the left/right
 * "thumbstick button", commonly shortened to LSB/RSB. Nintendo doesn't
 * brand them at all (Switch controller diagrams just show a stick-press
 * icon); LS/RS is the closest short, commonly understood label. */
var SHOULDER_LABELS = {
  xbox: { lb: 'LB', rb: 'RB', lt: 'LT', rt: 'RT', select: 'Back', start: 'Start', l3: 'LSB', r3: 'RSB' },
  playstation: { lb: 'L1', rb: 'R1', lt: 'L2', rt: 'R2', select: 'Share', start: 'Options', l3: 'L3', r3: 'R3' },
  nintendo: { lb: 'L', rb: 'R', lt: 'ZL', rt: 'ZR', select: '−', start: '+', l3: 'LS', r3: 'RS' }
};

var vgpFaceEls = {
  top: document.getElementById('vgp-face-top'),
  left: document.getElementById('vgp-face-left'),
  right: document.getElementById('vgp-face-right'),
  bottom: document.getElementById('vgp-face-bottom')
};
var vgpFaceContainer = document.getElementById('vgp-face');
var vgpLb = document.getElementById('vgp-lb');
var vgpRb = document.getElementById('vgp-rb');
var vgpLt = document.getElementById('vgp-lt');
var vgpRt = document.getElementById('vgp-rt');
var vgpSelectBtn = document.getElementById('vgp-select');
var vgpStartBtn = document.getElementById('vgp-start');
var vgpL3Btn = document.getElementById('vgp-l3');
var vgpR3Btn = document.getElementById('vgp-r3');
var vgpGuideBtn = document.getElementById('vgp-guide');

function applyVgpLayout(layout) {
  var skin = FACE_SKINS[layout] || FACE_SKINS.xbox;
  ['top', 'left', 'right', 'bottom'].forEach(function (pos) {
    var def = skin[pos];
    var el = vgpFaceEls[pos];
    el.textContent = def.label;
    el.style.background = def.color;
  });
  var labels = SHOULDER_LABELS[layout] || SHOULDER_LABELS.xbox;
  vgpLb.textContent = labels.lb;
  vgpRb.textContent = labels.rb;
  vgpLt.textContent = labels.lt;
  vgpRt.textContent = labels.rt;
  vgpSelectBtn.textContent = labels.select;
  vgpStartBtn.textContent = labels.start;
  vgpL3Btn.textContent = labels.l3;
  vgpR3Btn.textContent = labels.r3;
}

/* Symmetric (PlayStation-style: both sticks low, d-pad/face buttons
 * above them) vs. asymmetric (Xbox/Nintendo-style: left stick sits
 * ABOVE the d-pad instead). Only the left side ever swaps -- on real
 * pads of every brand the right side (face buttons above stick) is
 * already the same, so there is nothing to toggle there. Defaults to
 * whatever matches the chosen layout unless the user has explicitly
 * overridden it at least once (settings.stickSymmetric stays undefined
 * until then). */
function defaultSymmetric(layout) {
  return layout === 'playstation';
}
function applyStickSymmetry(symmetric) {
  vgpEl.classList.toggle('vgp-asym', !symmetric);
  vgpSymmetricBox.checked = symmetric;
}
vgpLayoutSelect.value = settings.vgpLayout || 'xbox';
applyVgpLayout(vgpLayoutSelect.value);
applyStickSymmetry(settings.stickSymmetric != null ? settings.stickSymmetric : defaultSymmetric(vgpLayoutSelect.value));
vgpLayoutSelect.onchange = function () {
  saveSettings({ vgpLayout: vgpLayoutSelect.value });
  applyVgpLayout(vgpLayoutSelect.value);
  if (settings.stickSymmetric == null) {
    applyStickSymmetry(defaultSymmetric(vgpLayoutSelect.value));
  }
};
vgpSymmetricBox.onchange = function () {
  saveSettings({ stickSymmetric: vgpSymmetricBox.checked });
  applyStickSymmetry(vgpSymmetricBox.checked);
};

/* --- free layout editing: "move buttons" lets the player drag any
 * control (or button cluster, for the d-pad/face group) to wherever
 * suits their hands/device, persisted in the same localStorage settings
 * blob as everything else on this page (so it survives reloads on this
 * browser, same mechanism as the volume/quality/etc. settings above). */
var vgpEditMode = false;
var vgpPositions = settings.vgpPositions || {};
var VGP_ANCHOR_IDS = [
  'vgp-lstick',
  'vgp-rstick',
  'vgp-dpad',
  'vgp-face',
  'vgp-l3',
  'vgp-r3',
  'vgp-lb',
  'vgp-lt',
  'vgp-rb',
  'vgp-rt',
  'vgp-select',
  'vgp-guide',
  'vgp-start'
];
function applyStoredPosition(el) {
  var pos = vgpPositions[el.id];
  if (!pos) return;
  el.style.left = pos.left;
  el.style.top = pos.top;
  el.style.right = 'auto';
  el.style.bottom = 'auto';
}
VGP_ANCHOR_IDS.forEach(function (id) {
  applyStoredPosition(document.getElementById(id));
});

/* Drags `el` as a whole (used directly for sticks/single buttons, and
 * with the shared `.vgp-face` container passed in for any of its 4
 * child buttons, so the diamond arrangement moves together). */
function startVgpDrag(el, e) {
  e.preventDefault();
  e.stopPropagation();
  el.setPointerCapture(e.pointerId);
  var rect = el.getBoundingClientRect();
  var offsetX = e.clientX - rect.left;
  var offsetY = e.clientY - rect.top;
  el.classList.add('dragging');
  function move(ev) {
    if (ev.pointerId !== e.pointerId) return;
    el.style.left = ev.clientX - offsetX + 'px';
    el.style.top = ev.clientY - offsetY + 'px';
    el.style.right = 'auto';
    el.style.bottom = 'auto';
  }
  function up(ev) {
    if (ev.pointerId !== e.pointerId) return;
    el.classList.remove('dragging');
    el.removeEventListener('pointermove', move);
    el.removeEventListener('pointerup', up);
    el.removeEventListener('pointercancel', up);
    vgpPositions[el.id] = { left: el.style.left, top: el.style.top };
    saveSettings({ vgpPositions: vgpPositions });
  }
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
}
vgpEditBox.checked = false;
vgpEditBox.onchange = function () {
  vgpEditMode = vgpEditBox.checked;
  vgpEl.classList.toggle('vgp-edit', vgpEditMode);
};
vgpResetBtn.onclick = function () {
  vgpPositions = {};
  saveSettings({ vgpPositions: {} });
  VGP_ANCHOR_IDS.forEach(function (id) {
    var el = document.getElementById(id);
    el.style.left = '';
    el.style.top = '';
    el.style.right = '';
    el.style.bottom = '';
  });
};

/* Generic press/release binding for a simple digital button. getIndices()
 * is called at press time (not bound once) so face buttons pick up
 * whichever slot the current layout assigns them. `dragEl` is the
 * element actually moved in edit mode -- the shared `.vgp-face`
 * container for the 4 face buttons, itself otherwise. */
function bindVgpButton(el, getIndices, dragEl) {
  el.addEventListener('pointerdown', function (e) {
    if (vgpEditMode) {
      startVgpDrag(dragEl || el, e);
      return;
    }
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    getIndices().forEach(function (i) {
      vgpTouchState[i] = 100;
    });
    /* Immediate feedback (zero-latency); the per-frame combiner below
     * confirms/maintains or clears this class every frame regardless, so
     * it also lights up correctly when a real gamepad presses it without
     * any touch involved. */
    el.classList.add('pressed');
  });
  function release() {
    getIndices().forEach(function (i) {
      vgpTouchState[i] = 0;
    });
  }
  el.addEventListener('pointerup', release);
  el.addEventListener('pointercancel', release);
}
bindVgpButton(vgpLb, function () {
  return [6];
});
bindVgpButton(vgpRb, function () {
  return [3];
});
bindVgpButton(vgpLt, function () {
  return [7];
});
bindVgpButton(vgpRt, function () {
  return [4];
});
bindVgpButton(vgpSelectBtn, function () {
  return [1];
});
bindVgpButton(vgpStartBtn, function () {
  return [2];
});
bindVgpButton(vgpGuideBtn, function () {
  return [0];
});

/* Declarative list used by the per-frame combiner below to toggle the
 * "pressed" class on every simple digital control (touch OR a real
 * gamepad, whichever is driving it) -- built once all the elements and
 * FACE_SLOTS exist. */
var VGP_DIGITAL_BINDINGS = [
  { el: vgpLb, idx: 6 },
  { el: vgpRb, idx: 3 },
  { el: vgpLt, idx: 7 },
  { el: vgpRt, idx: 4 },
  { el: vgpSelectBtn, idx: 1 },
  { el: vgpStartBtn, idx: 2 },
  { el: vgpGuideBtn, idx: 0 },
  { el: vgpL3Btn, idx: 8 },
  { el: vgpR3Btn, idx: 5 },
  { el: vgpFaceEls.top, idx: FACE_SLOTS.top },
  { el: vgpFaceEls.left, idx: FACE_SLOTS.left },
  { el: vgpFaceEls.right, idx: FACE_SLOTS.right },
  { el: vgpFaceEls.bottom, idx: FACE_SLOTS.bottom }
];
bindVgpButton(vgpL3Btn, function () {
  return [8];
});
bindVgpButton(vgpR3Btn, function () {
  return [5];
});
bindVgpButton(
  vgpFaceEls.top,
  function () {
    return [FACE_SLOTS.top];
  },
  vgpFaceContainer
);
bindVgpButton(
  vgpFaceEls.left,
  function () {
    return [FACE_SLOTS.left];
  },
  vgpFaceContainer
);
bindVgpButton(
  vgpFaceEls.right,
  function () {
    return [FACE_SLOTS.right];
  },
  vgpFaceContainer
);
bindVgpButton(
  vgpFaceEls.bottom,
  function () {
    return [FACE_SLOTS.bottom];
  },
  vgpFaceContainer
);

/* Draggable virtual stick: one finger captured per stick (a second finger
 * landing on an already-active stick is ignored), moved freely via
 * Pointer Events -- once captured, move/up events keep targeting this
 * element even if the finger slides outside its visible circle, so a
 * quick/wide swipe isn't lost. clientY grows downward, same sign
 * convention already used for the real gamepad's LY/RY axes (negative =
 * up), so no inversion is needed here.
 *
 * `key` ('l'/'r') records in vgpStickActive whether THIS stick currently
 * has a finger on it, so the per-frame combiner knows whether it's safe
 * to drive the knob's visual position from a real gamepad's axis instead
 * (never fight an actual finger for the knob's position; the two input
 * sources are still summed into the actual output regardless). */
var vgpStickActive = { l: false, r: false };
var vgpStickEls = {};
function bindVgpStick(el, ixX, ixY, key) {
  var knob = el.querySelector('.vgp-knob');
  vgpStickEls[key] = { el: el, knob: knob, ixX: ixX, ixY: ixY };
  var activePointer = null;
  function update(e) {
    var rect = el.getBoundingClientRect();
    var cx = rect.left + rect.width / 2;
    var cy = rect.top + rect.height / 2;
    var dx = e.clientX - cx;
    var dy = e.clientY - cy;
    var maxR = rect.width / 2;
    var dist = Math.sqrt(dx * dx + dy * dy);
    if (dist > maxR) {
      dx = (dx / dist) * maxR;
      dy = (dy / dist) * maxR;
    }
    knob.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
    vgpTouchState[ixX] = clamp100(dx / maxR);
    vgpTouchState[ixY] = clamp100(dy / maxR);
  }
  el.addEventListener('pointerdown', function (e) {
    if (vgpEditMode) {
      startVgpDrag(el, e);
      return;
    }
    e.preventDefault();
    if (activePointer !== null) return;
    activePointer = e.pointerId;
    vgpStickActive[key] = true;
    el.setPointerCapture(e.pointerId);
    update(e);
  });
  el.addEventListener('pointermove', function (e) {
    if (e.pointerId !== activePointer) return;
    update(e);
  });
  function release(e) {
    if (e.pointerId !== activePointer) return;
    activePointer = null;
    vgpStickActive[key] = false;
    vgpTouchState[ixX] = 0;
    vgpTouchState[ixY] = 0;
  }
  el.addEventListener('pointerup', release);
  el.addEventListener('pointercancel', release);
}
bindVgpStick(document.getElementById('vgp-lstick'), 11, 12, 'l'); // LX/LY
bindVgpStick(document.getElementById('vgp-rstick'), 9, 10, 'r'); // RX/RY

/* Single touch zone for the whole d-pad instead of 4 separate buttons:
 * up/down and left/right are each detected independently from the touch
 * position relative to the pad's center, so diagonals fall out for free
 * (e.g. up+right both end up active at once) instead of only ever the
 * single nearest direction. */
var vgpDpadArrows = null;
function bindVgpDpad(el) {
  var arrows = {
    up: el.querySelector('.vgp-dpad-up'),
    down: el.querySelector('.vgp-dpad-down'),
    left: el.querySelector('.vgp-dpad-left'),
    right: el.querySelector('.vgp-dpad-right')
  };
  vgpDpadArrows = arrows;
  var DEADZONE = 0.28;
  var activePointer = null;
  function setDir(index, on) {
    vgpTouchState[index] = on ? 100 : 0;
  }
  function update(e) {
    var rect = el.getBoundingClientRect();
    var dx = (e.clientX - (rect.left + rect.width / 2)) / (rect.width / 2);
    var dy = (e.clientY - (rect.top + rect.height / 2)) / (rect.height / 2);
    setDir(13, dy < -DEADZONE);
    setDir(14, dy > DEADZONE);
    setDir(15, dx < -DEADZONE);
    setDir(16, dx > DEADZONE);
  }
  function releaseAll() {
    setDir(13, false);
    setDir(14, false);
    setDir(15, false);
    setDir(16, false);
  }
  el.addEventListener('pointerdown', function (e) {
    if (vgpEditMode) {
      startVgpDrag(el, e);
      return;
    }
    e.preventDefault();
    if (activePointer !== null) return;
    activePointer = e.pointerId;
    el.setPointerCapture(e.pointerId);
    update(e);
  });
  el.addEventListener('pointermove', function (e) {
    if (e.pointerId !== activePointer) return;
    update(e);
  });
  function release(e) {
    if (e.pointerId !== activePointer) return;
    activePointer = null;
    releaseAll();
  }
  el.addEventListener('pointerup', release);
  el.addEventListener('pointercancel', release);
}
bindVgpDpad(document.getElementById('vgp-dpad'));

function setVirtualGamepadActive(active) {
  vgpEl.classList.toggle('hidden', !active);
  vgpLayoutRow.classList.toggle('hidden', !active);
  vgpSymmetricRow.classList.toggle('hidden', !active);
  vgpEditRow.classList.toggle('hidden', !active);
  vgpResetBtn.classList.toggle('hidden', !active);
  vgpColorRow.classList.toggle('hidden', !active);
  vgpOpacityRow.classList.toggle('hidden', !active);
  if (!active) {
    gamepadState.fill(0);
    vgpTouchState.fill(0);
    if (vgpEditMode) {
      vgpEditBox.checked = false;
      vgpEditMode = false;
      vgpEl.classList.remove('vgp-edit');
    }
  }
}

function setKeyboardModeActive(active) {
  if (!active) {
    gamepadState.fill(0);
    kbState.fill(0);
    kbHeldKeys = {};
    mouseStickX = 0;
    mouseStickY = 0;
    if (document.pointerLockElement === video) {
      document.exitPointerLock();
    }
  }
}

/* Refreshes the dropdown list from the gamepads actually detected by the
 * browser -- several can be connected at the same time, the user
 * explicitly picks which one to use rather than defaulting to the first
 * one found. */
function updateGamepadList() {
  /* A viewer never gets to choose a controller, so don't go looking for
   * one: the dropdown is hidden and the server would refuse whatever it
   * produced. Chrome also only reveals gamepad identities once they are
   * actually queried, so not querying is the quieter thing to do. */
  if (!playerControlsEnabled) {
    return;
  }
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  var connected = [];
  for (var i = 0; i < pads.length; i++) {
    if (pads[i]) connected.push(pads[i]);
  }

  gamepadSelect.innerHTML = '';
  var noneOpt = document.createElement('option');
  noneOpt.value = '';
  noneOpt.textContent = connected.length ? '(none chosen)' : 'none detected';
  gamepadSelect.appendChild(noneOpt);

  /* Always offered, whether or not the browser sees a real gamepad --
   * this is a touch overlay, not something Gamepad API detects. */
  var virtualOpt = document.createElement('option');
  virtualOpt.value = VIRTUAL_GAMEPAD_ID;
  virtualOpt.textContent = 'Virtual buttons (touch)';
  gamepadSelect.appendChild(virtualOpt);

  var keyboardOpt = document.createElement('option');
  keyboardOpt.value = KEYBOARD_GAMEPAD_ID;
  keyboardOpt.textContent = 'Keyboard/mouse';
  gamepadSelect.appendChild(keyboardOpt);

  var stillPresent = selectedGamepadId === VIRTUAL_GAMEPAD_ID || selectedGamepadId === KEYBOARD_GAMEPAD_ID;
  connected.forEach(function (gp) {
    var opt = document.createElement('option');
    opt.value = gp.id;
    opt.textContent = gp.id;
    if (gp.id === selectedGamepadId) {
      opt.selected = true;
      stillPresent = true;
    }
    gamepadSelect.appendChild(opt);
  });
  if (selectedGamepadId === VIRTUAL_GAMEPAD_ID || selectedGamepadId === KEYBOARD_GAMEPAD_ID) {
    gamepadSelect.value = selectedGamepadId;
  }

  /* If nothing is explicitly chosen and only one gamepad is plugged in,
   * pick it by default (the most common case) -- otherwise the user has
   * to choose themselves among several. */
  if (!selectedGamepadId && connected.length === 1) {
    selectedGamepadId = connected[0].id;
    gamepadSelect.value = selectedGamepadId;
  } else if (
    selectedGamepadId &&
    selectedGamepadId !== VIRTUAL_GAMEPAD_ID &&
    selectedGamepadId !== KEYBOARD_GAMEPAD_ID &&
    !stillPresent
  ) {
    currentGamepadStatus = 'disconnected (' + selectedGamepadId + ')';
  }
}
gamepadSelect.onchange = function () {
  selectedGamepadId = gamepadSelect.value || null;
  saveSettings({ gamepadId: selectedGamepadId });
  setVirtualGamepadActive(selectedGamepadId === VIRTUAL_GAMEPAD_ID);
  setKeyboardModeActive(selectedGamepadId === KEYBOARD_GAMEPAD_ID);
  updateRebindButtonVisibility();
  rebindEl.classList.add('hidden');
  rebindListening = null;
};
window.addEventListener('gamepadconnected', updateGamepadList);
window.addEventListener('gamepaddisconnected', updateGamepadList);
updateGamepadList();
setVirtualGamepadActive(selectedGamepadId === VIRTUAL_GAMEPAD_ID);
setKeyboardModeActive(selectedGamepadId === KEYBOARD_GAMEPAD_ID);
updateRebindButtonVisibility();

function getSelectedGamepad() {
  if (!selectedGamepadId) return null;
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    if (pads[i] && pads[i].id === selectedGamepadId) return pads[i];
  }
  return null;
}

/* Any real gamepad the browser currently sees, regardless of the
 * dropdown selection -- used only in "Virtual buttons" mode, where a
 * real controller (if plugged in) stays active alongside the touch
 * overlay rather than needing to be explicitly picked. Picks the first
 * one found; if more than one is connected while in virtual mode,
 * that's an edge case we don't try to disambiguate. */
function findAnyRealGamepad() {
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    if (pads[i]) return pads[i];
  }
  return null;
}

/* Connection time of each gamepad (by id), to ignore its very first
 * readings: many browsers only "discover" a gamepad the moment a button
 * is pressed on it, and that very first state (sometimes
 * unstable/stale for the first frames) used to be sent as-is -- observed
 * as a "phantom button" at the moment of detection. */
var gamepadConnectedAt = {};
window.addEventListener('gamepadconnected', function (e) {
  gamepadConnectedAt[e.gamepad.id] = performance.now();
});
var GAMEPAD_SETTLE_MS = 300;

/* --- per-real-gamepad rebind profiles: which browser button/axis index
 * feeds each GAMEPAD_XB360_* slot, stored per gamepad id
 * (settings.gamepadProfiles[gp.id]) since different controllers need
 * different treatment -- confirmed this session with the Xbox Series
 * X|S controller's own nonstandard browser button order, and expected
 * again with a PS5 DualSense (different, but standard-compliant,
 * order). defaultGamepadProfile() is exactly the mapping this file used
 * to have hardcoded, so a controller that's never been rebound behaves
 * exactly as before. */
var GAMEPAD_ROWS = [
  { field: 'guide', label: 'Guide', kind: 'button' },
  { field: 'back', label: 'Back', kind: 'button' },
  { field: 'start', label: 'Start', kind: 'button' },
  { field: 'rb', label: 'RB', kind: 'button' },
  { field: 'rt', label: 'RT', kind: 'trigger' },
  { field: 'rs', label: 'RS', kind: 'button' },
  { field: 'lb', label: 'LB', kind: 'button' },
  { field: 'lt', label: 'LT', kind: 'trigger' },
  { field: 'ls', label: 'LS', kind: 'button' },
  { field: 'rx', label: 'RX', kind: 'axis' },
  { field: 'ry', label: 'RY', kind: 'axis' },
  { field: 'lx', label: 'LX', kind: 'axis' },
  { field: 'ly', label: 'LY', kind: 'axis' },
  { field: 'up', label: 'Up', kind: 'button' },
  { field: 'down', label: 'Down', kind: 'button' },
  { field: 'left', label: 'Left', kind: 'button' },
  { field: 'right', label: 'Right', kind: 'button' },
  { field: 'y', label: 'Y', kind: 'button' },
  { field: 'b', label: 'B', kind: 'button' },
  { field: 'a', label: 'A', kind: 'button' },
  { field: 'x', label: 'X', kind: 'button' }
];

/* Whether this controller reports its top/left face buttons transposed.
 *
 * This was originally applied to every gamepad, on the belief that the
 * swap happened in the GCAPI translation to the console. It does not:
 * it is the controller misreporting itself. The Xbox Series X|S pad here
 * comes back with 18 buttons and 6 axes instead of the standard 17/4,
 * with X and Y transposed -- while a PS5 DualSense reports a proper
 * standard mapping, so compensating for it there is what inverted square
 * and triangle. Keyed on the controller so both are right untouched;
 * anything this guesses wrong stays fixable per-pad in the rebind panel.
 *
 * The underlying cause is likely the Linux xpad driver's button order,
 * so the same pad on another OS may well report standard mapping and not
 * want this. That case rebinds like any other. */
function gamepadNeedsXYSwap(gpId) {
  var id = String(gpId || '').toLowerCase();
  return id.indexOf('045e') !== -1 /* Microsoft vendor id */ || /xbox|xinput/.test(id);
}

function defaultGamepadProfile(gpId) {
  var swapXY = gamepadNeedsXYSwap(gpId);
  return {
    guide: { type: 'button', index: 16 },
    back: { type: 'button', index: 8 },
    start: { type: 'button', index: 9 },
    rb: { type: 'button', index: 5 },
    rt: { type: 'axis', index: 5, fallbackButton: 7 },
    rs: { type: 'button', index: 11 },
    lb: { type: 'button', index: 4 },
    lt: { type: 'axis', index: 4, fallbackButton: 6 },
    ls: { type: 'button', index: 10 },
    rx: { type: 'axis', index: 2 },
    ry: { type: 'axis', index: 3 },
    lx: { type: 'axis', index: 0 },
    ly: { type: 'axis', index: 1 },
    up: { type: 'button', index: 12 },
    down: { type: 'button', index: 13 },
    left: { type: 'button', index: 14 },
    right: { type: 'button', index: 15 },
    /* Standard mapping is 0=bottom, 1=right, 2=left, 3=top. Only pads
     * that transpose the two get them the other way round. */
    y: { type: 'button', index: swapXY ? 2 : 3 },
    b: { type: 'button', index: 1 },
    a: { type: 'button', index: 0 },
    x: { type: 'button', index: swapXY ? 3 : 2 }
  };
}
/* Defaults are immutable and depend only on the id, so they are built
 * once per controller rather than on every frame. buildStateFromGamepad()
 * asks for a profile 60 times a second, and a gamepad that has never been
 * rebound -- the normal case -- took the defaults branch every time,
 * allocating 22 objects plus a lowercased copy of the id for the
 * work of reading four button indices. */
var defaultProfileCache = {};

function getGamepadProfile(gpId) {
  var profiles = settings.gamepadProfiles || {};
  if (profiles[gpId]) return profiles[gpId];
  if (!defaultProfileCache[gpId]) {
    defaultProfileCache[gpId] = defaultGamepadProfile(gpId);
  }
  return defaultProfileCache[gpId];
}
function saveGamepadProfile(gpId, profile) {
  var profiles = settings.gamepadProfiles || {};
  profiles[gpId] = profile;
  saveSettings({ gamepadProfiles: profiles });
}

/* Builds the 21-value GAMEPAD_XB360_* state from a real browser Gamepad
 * object into `out` (any 21-length indexable, zeroed by the caller
 * first if a clean base is needed), using that gamepad's own rebind
 * profile. Shared by the normal "pick one real gamepad from the
 * dropdown" path, "Virtual buttons" mode's real-gamepad-stays-active
 * merge, and "Keyboard/mouse" mode's real-gamepad merge -- all three
 * behave identically (same invert-ry/threshold settings, same per-
 * gamepad rebind profile). */
function buildStateFromGamepad(gp, out) {
  var b = gp.buttons;
  var a = gp.axes;
  var profile = getGamepadProfile(gp.id);
  var digitalField = function (name) {
    var f = profile[name];
    return f && b[f.index] && b[f.index].pressed ? 100 : 0;
  };
  /* Read as a pair, because the limits apply to the vector rather than
   * to each axis: see stickPairToWire. */
  var stickPair = function (xName, yName, which) {
    var fx = profile[xName], fy = profile[yName];
    var rx = fx && a[fx.index] !== undefined ? a[fx.index] : 0;
    var ry = fy && a[fy.index] !== undefined ? a[fy.index] : 0;
    return stickPairToWire(rx, ry, which);
  };
  /* Triggers: on some gamepads (Xbox Series X|S observed here), LT/RT
   * do NOT come back as standard buttons but as extra axes (range
   * -1.00 to 1.00, 1.00 = fully pressed) -- read via the bound axis
   * first, thresholded by the adjustable sliders on the site, falling
   * back to a plain button read if the binding is a button instead
   * (either the profile's own fallbackButton, or a button the user
   * explicitly rebound this field to via the rebind panel). */
  var triggerField = function (name, threshold) {
    var f = profile[name];
    if (!f) return 0;
    if (f.type === 'axis' && a[f.index] !== undefined) {
      return a[f.index] > threshold ? 100 : 0;
    }
    var buttonIndex = f.type === 'button' ? f.index : f.fallbackButton;
    return buttonIndex != null && b[buttonIndex] && (b[buttonIndex].pressed || b[buttonIndex].value > 0.9) ? 100 : 0;
  };

  out[0] = digitalField('guide');
  out[1] = digitalField('back');
  out[2] = digitalField('start');
  out[3] = digitalField('rb');
  out[4] = triggerField('rt', Number(rtThresholdSlider.value));
  out[5] = digitalField('rs');
  out[6] = digitalField('lb');
  out[7] = triggerField('lt', Number(ltThresholdSlider.value));
  out[8] = digitalField('ls');
  var right = stickPair('rx', 'ry', 'right');
  var left = stickPair('lx', 'ly', 'left');
  out[9] = right[0];
  out[10] = invertRyBox.checked ? -right[1] : right[1];
  out[11] = left[0];
  out[12] = left[1];
  out[13] = digitalField('up');
  out[14] = digitalField('down');
  out[15] = digitalField('left');
  out[16] = digitalField('right');
  out[17] = digitalField('y');
  out[18] = digitalField('b');
  out[19] = digitalField('a');
  out[20] = digitalField('x');
}

var vgpRealState = new Int8Array(21);

/* Positions a stick's knob from a -100..100 value pair, used to reflect
 * a real gamepad's axis position on-screen while no finger is on that
 * particular stick (a finger, when present, already positions the knob
 * itself in bindVgpStick's own update()). */
function setVgpKnobFromValue(key, vx, vy) {
  var s = vgpStickEls[key];
  if (!s) return;
  var maxR = s.el.getBoundingClientRect().width / 2;
  s.knob.style.transform = 'translate(' + (vx / 100) * maxR + 'px,' + (vy / 100) * maxR + 'px)';
}

/* Combines touch input with a real gamepad's input (if any is connected)
 * into `gamepadState`, and keeps the overlay's own visuals (pressed
 * buttons, stick knob positions) in sync with whichever source is
 * driving them -- called once per frame, only in "Virtual buttons"
 * mode. Saturating add: either source alone passes through unchanged,
 * both together add up and clamp, same philosophy as the (server-side,
 * currently disabled) real-controller passthrough merge. */
function combineVirtualGamepadState() {
  var realGp = findAnyRealGamepad();
  vgpRealState.fill(0);
  if (realGp) {
    var settling =
      gamepadConnectedAt[realGp.id] && performance.now() - gamepadConnectedAt[realGp.id] < GAMEPAD_SETTLE_MS;
    if (!settling) buildStateFromGamepad(realGp, vgpRealState);
  }
  for (var i = 0; i < 21; i++) {
    gamepadState[i] = Math.max(-100, Math.min(100, vgpTouchState[i] + vgpRealState[i]));
  }
  VGP_DIGITAL_BINDINGS.forEach(function (bnd) {
    bnd.el.classList.toggle('pressed', gamepadState[bnd.idx] !== 0);
  });
  if (vgpDpadArrows) {
    vgpDpadArrows.up.classList.toggle('active', gamepadState[13] !== 0);
    vgpDpadArrows.down.classList.toggle('active', gamepadState[14] !== 0);
    vgpDpadArrows.left.classList.toggle('active', gamepadState[15] !== 0);
    vgpDpadArrows.right.classList.toggle('active', gamepadState[16] !== 0);
  }
  if (!vgpStickActive.l) setVgpKnobFromValue('l', gamepadState[11], gamepadState[12]);
  if (!vgpStickActive.r) setVgpKnobFromValue('r', gamepadState[9], gamepadState[10]);
  return realGp;
}

/* --- "Keyboard/mouse" mode: another special entry in the same gamepad
 * dropdown. Keys drive the d-pad, left stick (as WASD-style digital
 * extremes) and every button; the mouse (via Pointer Lock, engaged by
 * clicking the video) drives the right stick continuously with a
 * self-centering decay. Both merge additively with a real gamepad too,
 * same philosophy as the virtual touch overlay above. Bindings are
 * saved as named profiles in localStorage (settings.keyboardProfiles),
 * switchable on the fly from the rebind panel. */
var KB_ROWS = [
  { field: 'guide', label: 'Guide' },
  { field: 'back', label: 'Back' },
  { field: 'start', label: 'Start' },
  { field: 'rb', label: 'RB' },
  { field: 'rt', label: 'RT' },
  { field: 'rs', label: 'RS (stick click)' },
  { field: 'lb', label: 'LB' },
  { field: 'lt', label: 'LT' },
  { field: 'ls', label: 'LS (stick click)' },
  { field: 'rx-', label: 'RX left', slot: 9, value: -100 },
  { field: 'rx+', label: 'RX right', slot: 9, value: 100 },
  { field: 'ry-', label: 'RY up', slot: 10, value: -100 },
  { field: 'ry+', label: 'RY down', slot: 10, value: 100 },
  { field: 'lx-', label: 'LX left', slot: 11, value: -100 },
  { field: 'lx+', label: 'LX right', slot: 11, value: 100 },
  { field: 'ly-', label: 'LY up', slot: 12, value: -100 },
  { field: 'ly+', label: 'LY down', slot: 12, value: 100 },
  { field: 'up', label: 'Up' },
  { field: 'down', label: 'Down' },
  { field: 'left', label: 'Left' },
  { field: 'right', label: 'Right' },
  { field: 'y', label: 'Y' },
  { field: 'b', label: 'B' },
  { field: 'a', label: 'A' },
  { field: 'x', label: 'X' }
];
/* Digital (non-axis) rows resolve to their GAMEPAD_XB360_* slot at
 * value 100 through this table; the 8 stick rows above carry their own
 * slot/value directly since two different keys can drive the same axis
 * in opposite directions. */
var KB_DIGITAL_SLOTS = {
  guide: 0, back: 1, start: 2, rb: 3, rt: 4, rs: 5, lb: 6, lt: 7, ls: 8,
  up: 13, down: 14, left: 15, right: 16, y: 17, b: 18, a: 19, x: 20
};
function kbRowSlotValue(row) {
  if (row.slot !== undefined) return { slot: row.slot, value: row.value };
  return { slot: KB_DIGITAL_SLOTS[row.field], value: 100 };
}
/* A profile binds each action to a keyboard key AND (optionally) a
 * mouse input -- either one triggers it, so e.g. RT can sit on both a
 * key and the right mouse button. Mouse ids are the tokens in
 * MOUSE_INPUTS below. */
function defaultKeyboardProfile() {
  return {
    keys: {
      up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight',
      'ly-': 'KeyW', 'ly+': 'KeyS', 'lx-': 'KeyA', 'lx+': 'KeyD',
      a: 'Space', b: 'ShiftLeft', x: 'ControlLeft', y: 'KeyF',
      lb: 'KeyQ', rb: 'KeyE', lt: 'Digit1', rt: 'Digit3',
      start: 'Enter', back: 'Backspace', guide: 'Tab',
      ls: 'KeyC', rs: 'KeyV'
    },
    /* Shooter-style defaults, the obvious mapping for a mouse: aim with
     * the right stick (movement, handled separately), shoot with the
     * right trigger, aim-down-sights with the left. */
    mouse: {
      rt: 'mouse0',
      lt: 'mouse2'
    }
  };
}

/* Everything a mouse can be bound to, besides movement (which always
 * drives the right stick). Wheel notches are momentary: they have no
 * "release" event, so they are held for MOUSE_WHEEL_PULSE_MS. */
var MOUSE_INPUTS = [
  { id: '', label: '(none)' },
  { id: 'mouse0', label: 'Left click' },
  { id: 'mouse1', label: 'Middle click' },
  { id: 'mouse2', label: 'Right click' },
  { id: 'mouse3', label: 'Mouse 4 (back)' },
  { id: 'mouse4', label: 'Mouse 5 (forward)' },
  { id: 'wheelup', label: 'Wheel up' },
  { id: 'wheeldown', label: 'Wheel down' }
];
var MOUSE_WHEEL_PULSE_MS = 80;
function getKeyboardProfiles() {
  var profiles = settings.keyboardProfiles || { Default: defaultKeyboardProfile() };
  /* Be tolerant of a profile that predates the {keys,mouse} shape even
   * if migration didn't see it (hand-edited storage, say). */
  for (var name in profiles) {
    if (profiles[name] && !profiles[name].keys) {
      profiles[name] = { keys: profiles[name], mouse: {} };
    }
    if (!profiles[name].mouse) profiles[name].mouse = {};
  }
  return profiles;
}
function getActiveKeyboardProfileName() {
  var profiles = getKeyboardProfiles();
  var name = settings.activeKeyboardProfile;
  return name && profiles[name] ? name : Object.keys(profiles)[0];
}
function getActiveKeyboardProfile() {
  return getKeyboardProfiles()[getActiveKeyboardProfileName()];
}
function saveKeyboardProfiles(profiles, activeName) {
  var patch = { keyboardProfiles: profiles };
  if (activeName) patch.activeKeyboardProfile = activeName;
  saveSettings(patch);
}

var kbCodeToRow = {};   /* keyboard code  -> row */
var kbMouseToRow = {};  /* mouse input id -> row */
function rebuildKbCodeToRow() {
  kbCodeToRow = {};
  kbMouseToRow = {};
  var profile = getActiveKeyboardProfile();
  KB_ROWS.forEach(function (row) {
    var code = profile.keys[row.field];
    if (code) kbCodeToRow[code] = row;
    var mouse = profile.mouse[row.field];
    if (mouse) kbMouseToRow[mouse] = row;
  });
}
/* Mouse inputs currently held, same idea as kbHeldKeys. Wheel entries
 * are timestamps: they expire on their own (see recomputeKbState). */
var kbHeldMouse = {};

function recomputeKbState() {
  kbState.fill(0);
  var apply = function (row) {
    if (!row) return;
    var sv = kbRowSlotValue(row);
    kbState[sv.slot] = Math.max(-100, Math.min(100, kbState[sv.slot] + sv.value));
  };
  for (var code in kbHeldKeys) {
    if (kbHeldKeys[code]) apply(kbCodeToRow[code]);
  }
  var now = performance.now();
  for (var id in kbHeldMouse) {
    var held = kbHeldMouse[id];
    if (held === true) {
      apply(kbMouseToRow[id]);
    } else if (typeof held === 'number') {
      /* A wheel pulse: still counts until it times out. */
      if (now - held < MOUSE_WHEEL_PULSE_MS) {
        apply(kbMouseToRow[id]);
      } else {
        kbHeldMouse[id] = false;
      }
    }
  }
}
rebuildKbCodeToRow();

/* Mouse -> right stick (RX/RY), via Pointer Lock: click the video while
 * in keyboard mode to engage, Escape (browser default) or clicking
 * elsewhere releases it. Deltas accumulate then decay back to center
 * each frame when the mouse stops moving, for a rough self-centering
 * "flick" feel rather than an absolute position. */
var MOUSE_SENSITIVITY = 0.6;
var MOUSE_DECAY = 0.85;
function updateMouseStick() {
  mouseStickX *= MOUSE_DECAY;
  mouseStickY *= MOUSE_DECAY;
  if (Math.abs(mouseStickX) < 0.5) mouseStickX = 0;
  if (Math.abs(mouseStickY) < 0.5) mouseStickY = 0;
}
document.addEventListener('mousemove', function (e) {
  if (document.pointerLockElement !== video) return;
  mouseStickX = Math.max(-100, Math.min(100, mouseStickX + e.movementX * MOUSE_SENSITIVITY));
  mouseStickY = Math.max(-100, Math.min(100, mouseStickY + e.movementY * MOUSE_SENSITIVITY));
});
video.addEventListener('click', function () {
  if (selectedGamepadId === KEYBOARD_GAMEPAD_ID && document.pointerLockElement !== video) {
    video.requestPointerLock();
  }
});

/* True while the mouse is genuinely "in the game": the pointer is
 * locked to the video. Outside that, the cursor is a normal cursor and
 * clicks belong to whatever they land on. */
function mouseIsPlaying() {
  return document.pointerLockElement === video;
}

/* True if an event landed on the page's own UI (settings bar, rebind
 * panel) rather than on the stream. Input there must never reach the
 * console: pressing a key to type in a field, or clicking a button,
 * would otherwise also fire whatever that input is bound to. */
function isUiTarget(e) {
  var el = e.target;
  while (el) {
    if (el === bar || el === rebindEl) return true;
    el = el.parentElement;
  }
  return false;
}

/* Mouse buttons and wheel as bindable inputs. Bound to the whole
 * document rather than the video element: once pointer-locked the
 * events still target the document, and a click that started on the
 * video can end anywhere. */
function mouseInputActive(id, active) {
  if (!kbMouseToRow[id]) return false; /* not bound to anything */
  kbHeldMouse[id] = active;
  recomputeKbState();
  return true;
}

document.addEventListener('mousedown', function (e) {
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  /* Only while actually playing: with the pointer unlocked the cursor
   * is back and the menu has to stay clickable. preventDefault() here
   * on a menu click would swallow it entirely -- which is exactly what
   * made the settings bar unusable in this mode. */
  if (!mouseIsPlaying() || isUiTarget(e)) return;
  if (mouseInputActive('mouse' + e.button, true)) {
    e.preventDefault();
  }
});
/* Releases are always processed, even over the UI or after the pointer
 * was unlocked mid-click: skipping one would leave the button stuck
 * down on the console. */
document.addEventListener('mouseup', function (e) {
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  mouseInputActive('mouse' + e.button, false);
});
/* Any exit from pointer lock (Escape, alt-tab, clicking away) means the
 * player has stopped playing: drop every held mouse input rather than
 * leaving it pressed on the console. */
document.addEventListener('pointerlockchange', function () {
  if (!mouseIsPlaying()) {
    kbHeldMouse = {};
    recomputeKbState();
  }
});
/* Right-click must not open the context menu while playing. */
document.addEventListener('contextmenu', function (e) {
  if (selectedGamepadId === KEYBOARD_GAMEPAD_ID && document.pointerLockElement === video) {
    e.preventDefault();
  }
});
document.addEventListener('wheel', function (e) {
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  if (!mouseIsPlaying() || isUiTarget(e)) return; /* let the menu scroll */
  var id = e.deltaY < 0 ? 'wheelup' : 'wheeldown';
  if (!kbMouseToRow[id]) return;
  e.preventDefault();
  /* A wheel notch has no release event, so record when it happened and
   * let recomputeKbState() expire it after MOUSE_WHEEL_PULSE_MS. */
  kbHeldMouse[id] = performance.now();
  recomputeKbState();
}, { passive: false });

/* --- rebind panel: shared UI for "real gamepad" (button/axis capture)
 * and "keyboard/mouse" (key capture) modes. Opening it puts the chosen
 * field into a "listening" state; the next matching input (a new
 * button press / axis motion for gamepad, or any keydown for keyboard)
 * captures and saves it, then the panel re-renders. (DOM refs and the
 * rebindListening/rebindGpSnapshot state live near the top of the file,
 * alongside the other early state -- see the note there.) */
function updateRebindButtonVisibility() {
  var isRealGamepad =
    selectedGamepadId && selectedGamepadId !== VIRTUAL_GAMEPAD_ID && selectedGamepadId !== KEYBOARD_GAMEPAD_ID;
  rebindBtn.classList.toggle('hidden', !(isRealGamepad || selectedGamepadId === KEYBOARD_GAMEPAD_ID));
}

function gamepadInputLabel(f) {
  if (!f) return '(unbound)';
  if (f.type === 'axis') return 'axis ' + f.index;
  return 'button ' + f.index;
}

function renderRebindPanel() {
  rebindRowsEl.innerHTML = '';
  rebindProfileRow.innerHTML = '';
  var isKeyboard = selectedGamepadId === KEYBOARD_GAMEPAD_ID;

  if (isKeyboard) {
    rebindTitle.textContent = 'Rebind keyboard/mouse';
    rebindNote.textContent =
      'Each action can have a key AND a mouse input -- either one triggers it. ' +
      'Mouse movement always drives the right stick while pointer-locked (click the video to engage, Escape to release); ' +
      'the RX/RY rows are an optional key-based alternative to that.';

    var profiles = getKeyboardProfiles();
    var activeName = getActiveKeyboardProfileName();
    var select = document.createElement('select');
    Object.keys(profiles).forEach(function (name) {
      var opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      if (name === activeName) opt.selected = true;
      select.appendChild(opt);
    });
    select.onchange = function () {
      saveKeyboardProfiles(profiles, select.value);
      rebuildKbCodeToRow();
      renderRebindPanel();
    };
    var saveAsBtn = document.createElement('button');
    saveAsBtn.textContent = 'Save as...';
    saveAsBtn.onclick = function () {
      var name = prompt('New profile name:', activeName + ' copy');
      if (!name) return;
      profiles[name] = JSON.parse(JSON.stringify(profiles[activeName]));
      saveKeyboardProfiles(profiles, name);
      rebuildKbCodeToRow();
      renderRebindPanel();
    };
    var resetBtn = document.createElement('button');
    resetBtn.textContent = 'Reset to default';
    resetBtn.onclick = function () {
      profiles[activeName] = defaultKeyboardProfile();
      saveKeyboardProfiles(profiles, activeName);
      rebuildKbCodeToRow();
      renderRebindPanel();
    };
    rebindProfileRow.appendChild(select);
    rebindProfileRow.appendChild(saveAsBtn);
    rebindProfileRow.appendChild(resetBtn);

    var activeProfile = profiles[activeName];
    KB_ROWS.forEach(function (row) {
      var rowEl = document.createElement('div');
      rowEl.className = 'rebind-row';

      var nameEl = document.createElement('span');
      nameEl.className = 'rb-name';
      nameEl.textContent = row.label;

      /* --- keyboard half: shows the bound key, captured by listening --- */
      var valueEl = document.createElement('span');
      valueEl.className = 'rb-value';
      valueEl.textContent = activeProfile.keys[row.field] || '(no key)';
      var btn = document.createElement('button');
      var listening = rebindListening && rebindListening.kind === 'keyboard' && rebindListening.field === row.field;
      btn.textContent = listening ? 'press a key...' : 'key';
      if (listening) btn.classList.add('listening');
      btn.onclick = function () {
        rebindListening = { kind: 'keyboard', field: row.field };
        renderRebindPanel();
      };

      /* --- mouse half: a plain dropdown rather than a capture. Mouse
       * buttons are a short, fixed list, so picking one is quicker and
       * less error-prone than clicking to record it -- and it avoids the
       * awkwardness of "click to bind a click". --- */
      var mouseSel = document.createElement('select');
      mouseSel.className = 'rb-mouse';
      MOUSE_INPUTS.forEach(function (mi) {
        var opt = document.createElement('option');
        opt.value = mi.id;
        opt.textContent = mi.label;
        if ((activeProfile.mouse[row.field] || '') === mi.id) opt.selected = true;
        mouseSel.appendChild(opt);
      });
      mouseSel.onchange = function () {
        applyMouseBinding(row.field, mouseSel.value);
      };

      rowEl.appendChild(nameEl);
      rowEl.appendChild(valueEl);
      rowEl.appendChild(btn);
      rowEl.appendChild(mouseSel);
      rebindRowsEl.appendChild(rowEl);
    });
  } else {
    var gp = getSelectedGamepad();
    rebindTitle.textContent = 'Rebind gamepad' + (gp ? ' -- ' + gp.id : '');
    rebindNote.textContent = 'Click "listen" then press the button (or move the axis) on the real gamepad you want bound to that row.';
    if (!gp) {
      rebindNote.textContent = 'No gamepad currently selected/connected.';
      return;
    }
    var gpProfile = getGamepadProfile(gp.id);
    var resetGpBtn = document.createElement('button');
    resetGpBtn.textContent = 'Reset to default';
    resetGpBtn.onclick = function () {
      saveGamepadProfile(gp.id, defaultGamepadProfile(gp.id));
      renderRebindPanel();
    };
    rebindProfileRow.appendChild(resetGpBtn);

    GAMEPAD_ROWS.forEach(function (row) {
      var rowEl = document.createElement('div');
      rowEl.className = 'rebind-row';
      var nameEl = document.createElement('span');
      nameEl.className = 'rb-name';
      nameEl.textContent = row.label;
      var valueEl = document.createElement('span');
      valueEl.className = 'rb-value';
      valueEl.textContent = gamepadInputLabel(gpProfile[row.field]);
      var btn = document.createElement('button');
      var listening = rebindListening && rebindListening.kind === 'gamepad' && rebindListening.field === row.field;
      btn.textContent = listening ? 'press/move...' : 'listen';
      if (listening) btn.classList.add('listening');
      btn.onclick = function () {
        rebindListening = { kind: 'gamepad', field: row.field };
        rebindGpSnapshot = null;
        renderRebindPanel();
      };
      rowEl.appendChild(nameEl);
      rowEl.appendChild(valueEl);
      rowEl.appendChild(btn);
      rebindRowsEl.appendChild(rowEl);
    });
  }
}

rebindBtn.onclick = function () {
  rebindEl.classList.remove('hidden');
  renderRebindPanel();
};
rebindCloseBtn.onclick = function () {
  rebindEl.classList.add('hidden');
  rebindListening = null;
  rebindGpSnapshot = null;
};

function applyGamepadBinding(field, entry) {
  var gp = getSelectedGamepad();
  if (!gp) return;
  /* Copied before being changed: getGamepadProfile() may hand back the
   * shared cached defaults, and writing through that would quietly turn
   * "the defaults for this pad" into "whatever was last rebound". */
  var from = getGamepadProfile(gp.id);
  var profile = {};
  for (var k in from) profile[k] = from[k];
  profile[field] = entry;
  saveGamepadProfile(gp.id, profile);
  rebindListening = null;
  rebindGpSnapshot = null;
  renderRebindPanel();
}
function applyKeyboardBinding(field, code) {
  var profiles = getKeyboardProfiles();
  var activeName = getActiveKeyboardProfileName();
  profiles[activeName].keys[field] = code;
  saveKeyboardProfiles(profiles, activeName);
  rebuildKbCodeToRow();
  rebindListening = null;
  renderRebindPanel();
}

function applyMouseBinding(field, mouseId) {
  var profiles = getKeyboardProfiles();
  var activeName = getActiveKeyboardProfileName();
  if (mouseId) {
    profiles[activeName].mouse[field] = mouseId;
  } else {
    delete profiles[activeName].mouse[field];
  }
  saveKeyboardProfiles(profiles, activeName);
  rebuildKbCodeToRow();
  /* Any held state for the old binding would otherwise stay stuck. */
  kbHeldMouse = {};
  recomputeKbState();
  rebindListening = null;
  renderRebindPanel();
}

/* Polls the currently-selected real gamepad for a NEW button press or a
 * significant axis move (compared to a snapshot taken when listening
 * started), to capture it as the binding for rebindListening.field.
 * Called every frame from gamepadLoop() below; near-zero cost when not
 * actively rebinding. */
function pollGamepadRebind() {
  if (!rebindListening || rebindListening.kind !== 'gamepad') return;
  var gp = getSelectedGamepad();
  if (!gp) return;
  if (!rebindGpSnapshot) {
    rebindGpSnapshot = {
      buttons: gp.buttons.map(function (btn) {
        return btn.pressed;
      }),
      axes: gp.axes.slice()
    };
    return;
  }
  for (var i = 0; i < gp.buttons.length; i++) {
    if (gp.buttons[i].pressed && !rebindGpSnapshot.buttons[i]) {
      applyGamepadBinding(rebindListening.field, { type: 'button', index: i });
      return;
    }
  }
  for (var j = 0; j < gp.axes.length; j++) {
    if (Math.abs(gp.axes[j] - (rebindGpSnapshot.axes[j] || 0)) > 0.5) {
      applyGamepadBinding(rebindListening.field, { type: 'axis', index: j });
      return;
    }
  }
}

document.addEventListener('keydown', function (e) {
  if (rebindListening && rebindListening.kind === 'keyboard') {
    e.preventDefault();
    applyKeyboardBinding(rebindListening.field, e.code);
    return;
  }
  if (selectedGamepadId !== KEYBOARD_GAMEPAD_ID) return;
  /* Typing into the settings bar (a password prompt, a profile name)
   * or the rebind panel must not also press whatever that key is bound
   * to on the console. Only key PRESSES are filtered this way; releases
   * below are always processed, so a key held while playing and let go
   * over the UI can't stay stuck down. */
  if (isUiTarget(e)) return;
  if (kbCodeToRow[e.code]) e.preventDefault();
  if (kbHeldKeys[e.code]) return; // ignore key-repeat
  kbHeldKeys[e.code] = true;
  recomputeKbState();
});
document.addEventListener('keyup', function (e) {
  if (kbHeldKeys[e.code]) {
    kbHeldKeys[e.code] = false;
    recomputeKbState();
  }
});

/* Combines keyboard + mouse + a real gamepad (if any) the same way
 * combineVirtualGamepadState() combines touch + a real gamepad --
 * additive, saturating. Doesn't drive any on-screen visuals (there is
 * no overlay in this mode). */
function combineKeyboardGamepadState() {
  updateMouseStick();
  var realGp = findAnyRealGamepad();
  vgpRealState.fill(0);
  if (realGp) {
    var settling =
      gamepadConnectedAt[realGp.id] && performance.now() - gamepadConnectedAt[realGp.id] < GAMEPAD_SETTLE_MS;
    if (!settling) buildStateFromGamepad(realGp, vgpRealState);
  }
  for (var i = 0; i < 21; i++) {
    gamepadState[i] = Math.max(-100, Math.min(100, kbState[i] + vgpRealState[i]));
  }
  gamepadState[9] = Math.max(-100, Math.min(100, gamepadState[9] + Math.round(mouseStickX)));
  gamepadState[10] = Math.max(-100, Math.min(100, gamepadState[10] + Math.round(mouseStickY)));
  return realGp;
}

function sendGamepadState() {
  /* While the rebind panel is open, every press is meant for it --
   * forwarding them would also fire them in the game, so mapping "A"
   * would press whatever A currently is on the console.
   *
   * Gated on the panel being OPEN, not on a capture being in progress:
   * clicking "listen" and then doing something else leaves the capture
   * armed, and gating on that left the controls dead until the panel
   * was closed. Closing the panel now always restores control.
   *
   * A zeroed state is sent rather than nothing at all: a button held
   * when the panel opened has to be released, not left stuck at its
   * last value. */
  if (!rebindEl.classList.contains('hidden')) {
    gamepadState.fill(0);
    sendGamepadBuffer();
    currentGamepadStatus = 'rebinding (input paused)';
    return;
  }
  if (selectedGamepadId === VIRTUAL_GAMEPAD_ID) {
    var realGp = combineVirtualGamepadState();
    currentGamepadStatus =
      'virtual buttons (touch, ' + vgpLayoutSelect.value + ' layout)' + (realGp ? ' + ' + realGp.id : '');
    if (!gpDebugEl.classList.contains('hidden')) {
      gpDebugEl.textContent = 'virtual[' + Array.prototype.join.call(gamepadState, ' ') + ']';
    }
    sendGamepadBuffer();
    return;
  }
  if (selectedGamepadId === KEYBOARD_GAMEPAD_ID) {
    var kbRealGp = combineKeyboardGamepadState();
    /* Say plainly how to get the cursor back: while the pointer is
     * locked the settings bar cannot be reached at all, and nothing else
     * on screen hints that Escape is the way out. */
    currentGamepadStatus =
      'keyboard/mouse (' + getActiveKeyboardProfileName() + ')' +
      (mouseIsPlaying() ? ' [Esc frees the cursor]' : ' [click the video to capture the mouse]') +
      (kbRealGp ? ' + ' + kbRealGp.id : '');
    if (!gpDebugEl.classList.contains('hidden')) {
      gpDebugEl.textContent = 'keyboard[' + Array.prototype.join.call(gamepadState, ' ') + ']';
    }
    sendGamepadBuffer();
    return;
  }
  var gp = getSelectedGamepad();
  currentGamepadStatus = gp
    ? gp.id + ' (mapping:' + (gp.mapping || 'none') + ' b:' + gp.buttons.length + ' a:' + gp.axes.length + ')'
    : selectedGamepadId
      ? 'disconnected (' + selectedGamepadId + ')'
      : 'none';
  /* Raw dump of the browser's buttons/axes: used to diagnose non-standard
   * gamepads or indices that don't match what we assume (e.g. a "select"
   * button that appears permanently pressed). */
  /* Only worth building when someone can actually read it. This runs 60
   * times a second: two map+join passes, a toFixed() per axis and a DOM
   * write, all of it discarded while the stats row is collapsed -- which
   * is its default state. */
  if (!gpDebugEl.classList.contains('hidden')) {
    if (gp) {
      var btnDump = gp.buttons
        .map(function (btn, i) {
          return i + ':' + (btn.pressed ? '1' : '0');
        })
        .join(' ');
      var axDump = gp.axes
        .map(function (v, i) {
          return i + ':' + v.toFixed(2);
        })
        .join(' ');
      gpDebugEl.textContent = 'buttons[' + btnDump + '] axes[' + axDump + ']';
    } else {
      gpDebugEl.textContent = '';
    }
  }
  if (!gamepadChannel || gamepadChannel.readyState !== 'open') return;
  var s = gamepadState;
  s.fill(0);
  var settling =
    gp && gamepadConnectedAt[gp.id] && performance.now() - gamepadConnectedAt[gp.id] < GAMEPAD_SETTLE_MS;
  if (gp && !settling) {
    buildStateFromGamepad(gp, s);
  }
  sendGamepadBuffer();
}

/* Only runs for a player. A viewer has nothing to send, so the whole
 * loop -- polling the Gamepad API every frame, reading the keyboard and
 * mouse state, touching the DataChannel -- is stopped outright rather
 * than run and thrown away. Restarted the moment a login succeeds. */
var gamepadLoopId = null;

function setGamepadLoopRunning(on) {
  /* Loose comparison on purpose: this var is declared further down the
   * file, so an early caller would see `undefined` rather than null and
   * a strict check would quietly refuse to start the loop. */
  if (on && gamepadLoopId == null) {
    gamepadLoopId = requestAnimationFrame(gamepadLoop);
  } else if (!on && gamepadLoopId != null) {
    cancelAnimationFrame(gamepadLoopId);
    gamepadLoopId = null;
  }
}

function gamepadLoop() {
  pollGamepadRebind();
  sendGamepadState();
  if (padTestIsOpen()) {
    /* After sendGamepadState(), so the diagram shows exactly the values
     * that WOULD be sent: while blocking, zeros go on the wire but
     * `gamepadState` still holds the real reading, which is what the
     * diagram must show. */
    updatePadTest();
  }
  gamepadLoopId = requestAnimationFrame(gamepadLoop);
}

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

/* ===================== controller test popup =========================
 *
 * Draws a controller and lights up whatever is currently pressed, in
 * one of three familiar shapes. Three deliberate choices:
 *
 * - It reflects `gamepadState`, i.e. the 21 values actually being SENT
 *   to the console, not the raw browser Gamepad API. So it tests the
 *   whole chain -- device, mapping, rebinds, virtual pad, keyboard --
 *   rather than just whether the browser sees a controller. If a
 *   button lights up here, that is what the console receives.
 *
 * - The layouts differ in more than labels. Nintendo's face buttons are
 *   a diagonal mirror of Xbox's (X on top, B at the bottom, A on the
 *   right), so the same physical press lands on a different position;
 *   PlayStation keeps Xbox's positions with its own glyphs.
 *
 * - Input is blocked by default while the popup is open (a checkbox
 *   lets it through): pressing every button to check them shouldn't
 *   also press them in the running game. */

var padTestEl = document.getElementById('padtest');
var padTestBtn = document.getElementById('padtest-btn');
var padTestClose = document.getElementById('padtest-close');
var padTestLayout = document.getElementById('padtest-layout');
var padTestBlock = document.getElementById('padtest-block');
var padTestSvg = document.getElementById('padtest-svg');
var padTestReadout = document.getElementById('padtest-readout');
var padTestSource = document.getElementById('padtest-source');

/* Slots, from gamepad_bridge.h's GAMEPAD_XB360_* order. */
var PT = { GUIDE: 0, BACK: 1, START: 2, RB: 3, RT: 4, RS: 5, LB: 6, LT: 7, LS: 8,
           RX: 9, RY: 10, LX: 11, LY: 12, UP: 13, DOWN: 14, LEFT: 15, RIGHT: 16,
           Y: 17, B: 18, A: 19, X: 20 };

/* A controller silhouette: central body plus two grips. Shared by every
 * layout -- only the controls on top of it move. */
/* Traced clockwise from the top centre. The notch between the grips has
 * to stay LOW (y~182): a shallower one cuts straight through where the
 * d-pad and the right stick sit, leaving them floating outside the
 * silhouette. */
var PAD_BODY =
  'M 200,58 C 252,58 288,50 324,50 C 364,50 390,84 388,134 ' +
  'C 386,186 370,224 338,224 C 312,224 296,206 288,188 ' +
  'C 280,184 250,182 200,182 C 150,182 120,184 112,188 ' +
  'C 104,206 88,224 62,224 C 30,224 14,186 12,134 ' +
  'C 10,84 36,50 76,50 C 112,50 148,58 200,58 Z';

var PAD_LAYOUTS = {
  xbox: {
    name: 'Xbox',
    lstick: { x: 104, y: 100 }, rstick: { x: 252, y: 156 },
    dpad: { x: 152, y: 156 }, faceCx: 300, faceCy: 104,
    face: { top: { slot: PT.Y, label: 'Y', color: '#c99a00' },
            left: { slot: PT.X, label: 'X', color: '#2f6fd0' },
            right: { slot: PT.B, label: 'B', color: '#c73a3a' },
            bottom: { slot: PT.A, label: 'A', color: '#3a9a4a' } },
    small: [ { slot: PT.BACK, x: 176, y: 106, label: '\u29c9' },
             { slot: PT.START, x: 224, y: 106, label: '\u2261' },
             { slot: PT.GUIDE, x: 200, y: 80, label: '\u2302', r: 12 } ],
    shoulder: { lb: 'LB', rb: 'RB', lt: 'LT', rt: 'RT' }
  },
  switch: {
    name: 'Switch Pro',
    lstick: { x: 104, y: 100 }, rstick: { x: 252, y: 156 },
    dpad: { x: 152, y: 156 }, faceCx: 300, faceCy: 104,
    /* Diagonal mirror of Xbox: X up, B down, A right, Y left. */
    face: { top: { slot: PT.X, label: 'X', color: '#2f6fd0' },
            left: { slot: PT.Y, label: 'Y', color: '#2f9a5a' },
            right: { slot: PT.A, label: 'A', color: '#c73a3a' },
            bottom: { slot: PT.B, label: 'B', color: '#d0b400' } },
    small: [ { slot: PT.BACK, x: 176, y: 106, label: '\u2212' },
             { slot: PT.START, x: 224, y: 106, label: '+' },
             { slot: PT.GUIDE, x: 200, y: 80, label: '\u2302', r: 12 } ],
    shoulder: { lb: 'L', rb: 'R', lt: 'ZL', rt: 'ZR' }
  },
  ps5: {
    name: 'DualSense',
    /* Symmetric sticks, d-pad up on the left. */
    lstick: { x: 158, y: 158 }, rstick: { x: 246, y: 158 },
    dpad: { x: 104, y: 100 }, faceCx: 300, faceCy: 104,
    face: { top: { slot: PT.Y, label: '\u25b3', color: '#3aada3' },
            left: { slot: PT.X, label: '\u25a1', color: '#c774b0' },
            right: { slot: PT.B, label: '\u25cb', color: '#c75a5a' },
            bottom: { slot: PT.A, label: '\u2715', color: '#4a7fd0' } },
    small: [ { slot: PT.BACK, x: 168, y: 96, label: '\u29c9' },
             { slot: PT.START, x: 236, y: 96, label: '\u2261' },
             { slot: PT.GUIDE, x: 202, y: 128, label: 'PS', r: 11 } ],
    shoulder: { lb: 'L1', rb: 'R1', lt: 'L2', rt: 'R2' }
  }
};

var padTestNodes = null; /* built per layout, then only updated */

function svgEl(tag, attrs) {
  var el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (var k in attrs) el.setAttribute(k, attrs[k]);
  return el;
}

function buildPadTest(layoutName) {
  var L = PAD_LAYOUTS[layoutName] || PAD_LAYOUTS.xbox;
  padTestSvg.innerHTML = '';
  var n = { buttons: [], sticks: {}, triggers: {} };

  padTestSvg.appendChild(svgEl('path', { d: PAD_BODY, class: 'pt-body' }));

  /* A round or rect button plus its label, remembered for updates. */
  function addButton(slot, x, y, r, label, color, shape, w, h) {
    var el;
    if (shape === 'rect') {
      el = svgEl('rect', { x: x - w / 2, y: y - h / 2, width: w, height: h, rx: h / 2,
                           class: 'pt-btn', fill: color || '#ffffff' });
    } else {
      el = svgEl('circle', { cx: x, cy: y, r: r, class: 'pt-btn', fill: color || '#ffffff' });
    }
    padTestSvg.appendChild(el);
    var t = svgEl('text', { x: x, y: y, class: 'pt-label' });
    t.textContent = label;
    padTestSvg.appendChild(t);
    n.buttons.push({ slot: slot, el: el, label: t });
  }

  /* Sticks: a well with a knob that follows the axes, and lights up on
   * click (L3/R3). */
  function addStick(key, cfg, xSlot, ySlot, clickSlot, name) {
    padTestSvg.appendChild(svgEl('circle', { cx: cfg.x, cy: cfg.y, r: 24, class: 'pt-well' }));
    var knob = svgEl('circle', { cx: cfg.x, cy: cfg.y, r: 13, class: 'pt-knob' });
    padTestSvg.appendChild(knob);
    var t = svgEl('text', { x: cfg.x, y: cfg.y + 36, class: 'pt-label' });
    t.textContent = name;
    padTestSvg.appendChild(t);
    n.sticks[key] = { knob: knob, cx: cfg.x, cy: cfg.y, xSlot: xSlot, ySlot: ySlot, clickSlot: clickSlot };
  }

  /* Triggers are analog: the fill height tracks the value. */
  function addTrigger(key, slot, x, label) {
    var top = 10, h = 28, w = 36;
    padTestSvg.appendChild(svgEl('rect', { x: x - w / 2, y: top, width: w, height: h, rx: 6, class: 'pt-well' }));
    var fill = svgEl('rect', { x: x - w / 2, y: top + h, width: w, height: 0, rx: 6, class: 'pt-trigger-fill' });
    padTestSvg.appendChild(fill);
    var t = svgEl('text', { x: x, y: top + h / 2, class: 'pt-label' });
    t.textContent = label;
    padTestSvg.appendChild(t);
    n.triggers[key] = { fill: fill, slot: slot, top: top, h: h };
  }

  /* Triggers sit above the shoulders, which in turn straddle the body's
   * top edge -- the schematic "seen from above and behind" arrangement. */
  addTrigger('lt', PT.LT, 80, L.shoulder.lt);
  addTrigger('rt', PT.RT, 320, L.shoulder.rt);
  addButton(PT.LB, 80, 48, 0, L.shoulder.lb, '#ffffff', 'rect', 48, 16);
  addButton(PT.RB, 320, 48, 0, L.shoulder.rb, '#ffffff', 'rect', 48, 16);

  /* D-pad: four arms around its centre. */
  var d = L.dpad, arm = 15, thick = 13;
  addButton(PT.UP, d.x, d.y - arm, 0, '▲', '#ffffff', 'rect', thick, 20);
  addButton(PT.DOWN, d.x, d.y + arm, 0, '▼', '#ffffff', 'rect', thick, 20);
  addButton(PT.LEFT, d.x - arm, d.y, 0, '◀', '#ffffff', 'rect', 20, thick);
  addButton(PT.RIGHT, d.x + arm, d.y, 0, '▶', '#ffffff', 'rect', 20, thick);

  var fr = 26;
  addButton(L.face.top.slot, L.faceCx, L.faceCy - fr, 13, L.face.top.label, L.face.top.color);
  addButton(L.face.bottom.slot, L.faceCx, L.faceCy + fr, 13, L.face.bottom.label, L.face.bottom.color);
  addButton(L.face.left.slot, L.faceCx - fr, L.faceCy, 13, L.face.left.label, L.face.left.color);
  addButton(L.face.right.slot, L.faceCx + fr, L.faceCy, 13, L.face.right.label, L.face.right.color);

  L.small.forEach(function (s) {
    addButton(s.slot, s.x, s.y, s.r || 9, s.label, '#ffffff');
  });

  addStick('l', L.lstick, PT.LX, PT.LY, PT.LS, 'L3');
  addStick('r', L.rstick, PT.RX, PT.RY, PT.RS, 'R3');

  padTestNodes = n;
}

/* Called every frame while the popup is open. */
function updatePadTest() {
  if (!padTestNodes) return;
  var s = gamepadState;

  padTestNodes.buttons.forEach(function (b) {
    b.el.classList.toggle('on', s[b.slot] !== 0);
  });

  for (var k in padTestNodes.sticks) {
    var st = padTestNodes.sticks[k];
    /* 24 is the well radius, 13 the knob's: keep the knob inside. */
    st.knob.setAttribute('cx', st.cx + (s[st.xSlot] / 100) * 11);
    st.knob.setAttribute('cy', st.cy + (s[st.ySlot] / 100) * 11);
    st.knob.classList.toggle('on', s[st.clickSlot] !== 0);
  }

  for (var t in padTestNodes.triggers) {
    var tr = padTestNodes.triggers[t];
    var frac = Math.min(1, Math.abs(s[tr.slot]) / 100);
    tr.fill.setAttribute('y', tr.top + tr.h * (1 - frac));
    tr.fill.setAttribute('height', tr.h * frac);
  }

  /* Raw values, so an axis that drifts or a button stuck at a partial
   * value is visible rather than merely "not lighting up". */
  var active = [];
  for (var i = 0; i < s.length; i++) {
    if (s[i] !== 0) active.push(PT_NAMES[i] + '=' + s[i]);
  }
  padTestReadout.textContent = active.length ? active.join('  ') : '(nothing pressed)';
  /* Composed fresh each frame. Appending the suffix to
   * currentGamepadStatus instead would re-append it on every one of the
   * 60 frames per second, growing the line without bound. */
  padTestSource.textContent = 'source: ' + currentGamepadStatus +
    (inputIsSuppressed() ? ' -- input blocked while testing' : '');
}

var PT_NAMES = ['GUIDE','BACK','START','RB','RT','RS','LB','LT','LS','RX','RY','LX','LY',
                'UP','DOWN','LEFT','RIGHT','Y','B','A','X'];

function padTestIsOpen() {
  return !padTestEl.classList.contains('hidden');
}

padTestBtn.onclick = function () {
  padTestEl.classList.remove('hidden');
  buildPadTest(padTestLayout.value);
};
padTestClose.onclick = function () {
  padTestEl.classList.add('hidden');
};
padTestLayout.value = settings.padTestLayout || 'xbox';
padTestLayout.onchange = function () {
  saveSettings({ padTestLayout: padTestLayout.value });
  buildPadTest(padTestLayout.value);
};
