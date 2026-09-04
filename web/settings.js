/*
 * capture2cloud -- what the browser remembers
 *
 * One JSON blob under one key, versioned, with a migration for every
 * change of shape. Loaded before anything that reads a preference,
 * which is nearly everything.
 */

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
var SETTINGS_VERSION = 4;

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
  /* 3 -> 4: the top of the slider went from four times the stream's own
   * level to eight, so the same position is now twice as loud. Halved,
   * or everyone's sound would double the next time the page loads. */
  if (version < 4 && typeof stored.volume === 'number') {
    stored.volume = Math.round(Math.min(100, Math.max(0, stored.volume)) / 2);
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
