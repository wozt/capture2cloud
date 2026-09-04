// Minimal DOM/browser stub to actually EXECUTE app.js under Node and
// catch runtime errors (ordering bugs, undefined refs) that a pure
// `node --check` syntax check can't see. Exported as a factory so
// multiple test scripts can build fresh sandboxes.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

function makeClassList() {
  const set = new Set();
  return {
    add: (...c) => c.forEach((x) => set.add(x)),
    remove: (...c) => c.forEach((x) => set.delete(x)),
    toggle: (c, on) => (on ? set.add(c) : set.delete(c)),
    contains: (c) => set.has(c)
  };
}

function makeElement(tag) {
  const listeners = {};
  const el = {
    tagName: tag,
    id: '',
    className: '',
    value: '',
    checked: false,
    textContent: '',
    style: {
      setProperty(k, v) {
        this[k] = v;
      }
    },
    classList: makeClassList(),
    children: [],
    dataset: {},
    innerHTML: '',
    disabled: false,
    // In a real page every element in the controls row sits inside a
    // <label>; app.js uses that to show/hide a control with its label.
    // The created parent gets an explicitly NULL parent of its own:
    // code that walks up the tree (isUiTarget) relies on the chain
    // ending, which a real DOM guarantees and a lazily-creating stub
    // otherwise would not -- that difference hung the suite until the
    // process ran out of memory.
    get parentElement() {
      if (this._parent === undefined) {
        this._parent = makeElement('label');
        this._parent._parent = null;
      }
      return this._parent;
    },
    appendChild(child) {
      this.children.push(child);
      return child;
    },
    removeChild() {},
    addEventListener(type, fn) {
      (listeners[type] = listeners[type] || []).push(fn);
    },
    removeEventListener(type, fn) {
      if (listeners[type]) listeners[type] = listeners[type].filter((f) => f !== fn);
    },
    dispatch(type, evt) {
      (listeners[type] || []).forEach((fn) => fn(evt));
    },
    setPointerCapture() {},
    getBoundingClientRect() {
      return { left: 0, top: 0, width: 130, height: 130 };
    },
    querySelector(sel) {
      const cls = sel.replace('.', '');
      function search(node) {
        if (!node.children) return null;
        for (const c of node.children) {
          if (c.classList && c.classList.contains(cls)) return c;
          const r = search(c);
          if (r) return r;
        }
        return null;
      }
      return search(el) || makeElement('div');
    },
    getContext() {
      return { drawImage() {} };
    },
    play() {
      return Promise.resolve();
    },
    requestPointerLock() {},
    requestFullscreen() {
      return Promise.resolve();
    }
  };
  return el;
}

// `initialStorage` / `initialSessionStorage` (both optional) pre-seed
// localStorage / sessionStorage before app.js runs, so tests can
// exercise how it reads pre-existing (or legacy, or corrupt) stored
// settings and a leftover player token.
function createSandbox(srcPath, initialStorage, initialSessionStorage) {
  const idMap = {};
  function el(id) {
    if (!idMap[id]) idMap[id] = makeElement('div');
    idMap[id].id = id;
    return idMap[id];
  }

  /* srcPath may be one file or the ordered list the page loads.
   *
   * The front end is split across web/, but it is still ONE program in
   * one scope -- plain scripts, not modules -- so the way to run it
   * here is the way the browser runs it: concatenated in the order
   * page.html lists them. Joining them also means this suite keeps
   * catching load-order mistakes, which is the failure a split like
   * this one actually risks: a function declaration hoists within a
   * file and not across two. */
  const paths = Array.isArray(srcPath) ? srcPath : [srcPath];
  const src = paths.map((p) => fs.readFileSync(p, 'utf8')).join('\n');

  /* Elements start with the classes page.html gives them.
   *
   * Two panels are written `class="hidden"` in the markup and are
   * therefore shut before a line of script runs. The stub used to
   * create every element bare, so "is the pad test closed on load?"
   * was answered not by the page but by whichever piece of script
   * happened to hide it on the way past -- which made the answer
   * depend on file order, and a split of this front end changed it.
   * Reading the markup is both more faithful and no longer a trap. */
  const html = fs.readFileSync(path.join(__dirname, '..', 'page.html'), 'utf8');
  const tagRe = /<[a-zA-Z][^>]*>/g;
  let tag;
  while ((tag = tagRe.exec(html))) {
    const id = /\sid="([^"]+)"/.exec(tag[0]);
    const cls = /\sclass="([^"]+)"/.exec(tag[0]);
    if (id && cls) {
      const e = el(id[1]);
      cls[1].trim().split(/\s+/).forEach((c) => { if (c) e.classList.add(c); });
      e.className = cls[1];
    }
  }
  const ids = new Set();
  const re = /getElementById\(['"]([^'"]+)['"]\)/g;
  let m;
  while ((m = re.exec(src))) ids.add(m[1]);
  ids.forEach((id) => el(id));

  /* Seed each element's initial classes from the real page.html.
   * Without this, anything the markup starts as `class="hidden"` looked
   * VISIBLE under test -- so a popup that ships closed appeared open,
   * and any logic keyed on that was tested against the wrong state. */
  try {
    const html = fs.readFileSync(path.join(path.dirname(srcPath), 'page.html'), 'utf8');
    const tagRe = /<[a-zA-Z][^>]*>/g;
    let tag;
    while ((tag = tagRe.exec(html))) {
      const idM = /\sid="([^"]+)"/.exec(tag[0]);
      const clsM = /\sclass="([^"]+)"/.exec(tag[0]);
      if (idM && clsM) {
        clsM[1].split(/\s+/).filter(Boolean).forEach((c) => el(idM[1]).classList.add(c));
      }
    }
  } catch (e) {
    /* page.html not next to app.js: leave classes empty rather than fail */
  }

  const docListeners = {};
  const documentStub = {
    getElementById: (id) => el(id),
    addEventListener(type, fn) {
      (docListeners[type] = docListeners[type] || []).push(fn);
    },
    removeEventListener(type, fn) {
      if (docListeners[type]) docListeners[type] = docListeners[type].filter((f) => f !== fn);
    },
    dispatch(type, evt) {
      (docListeners[type] || []).forEach((fn) => fn(evt));
    },
    createElement: (tag) => makeElement(tag),
    /* app.js builds the controller diagram with SVG elements. */
    createElementNS: (ns, tag) => {
      const e = makeElement(tag);
      e.setAttribute = function (k, v) {
        this[k] = v;
      };
      e.getAttribute = function (k) {
        return this[k];
      };
      return e;
    },
    fullscreenElement: null,
    pointerLockElement: null,
    exitPointerLock() {},
    exitFullscreen() {
      return Promise.resolve();
    }
  };

  const localStorageStub = (() => {
    let store = Object.assign({}, initialStorage);
    return {
      getItem: (k) => (Object.prototype.hasOwnProperty.call(store, k) ? store[k] : null),
      setItem: (k, v) => {
        store[k] = String(v);
      },
      removeItem: (k) => {
        delete store[k];
      }
    };
  })();

  const sandbox = {
    console,
    window: {
      addEventListener(type, fn) {
        (this._l = this._l || {})[type] = (this._l[type] || []).concat(fn);
      },
      dispatch(type, evt) {
        ((this._l || {})[type] || []).forEach((fn) => fn(evt));
      },
      /* Enough of Web Audio to let both output routes be built. Without
       * it the low-latency graph could never exist under test, and a
       * mute bug that only appears when BOTH routes are live has nowhere
       * to show itself. */
      AudioContext: function () {
        return {
          state: 'running',
          baseLatency: 0.005,
          destination: {},
          createMediaStreamSource: () => ({ connect() {} }),
          createMediaElementSource: () => ({ connect() {} }),
          createGain: () => ({ gain: { value: 1 }, connect() {} }),
          resume: () => Promise.resolve(),
          close: () => Promise.resolve()
        };
      },
      innerHeight: 800
    },
    document: documentStub,
    localStorage: localStorageStub,
    // Separate from localStorage on purpose: app.js keeps the player
    // session token here so it dies with the tab.
    sessionStorage: (() => {
      let store = Object.assign({}, initialSessionStorage);
      return {
        getItem: (k) => (Object.prototype.hasOwnProperty.call(store, k) ? store[k] : null),
        setItem: (k, v) => {
          store[k] = String(v);
        },
        removeItem: (k) => {
          delete store[k];
        }
      };
    })(),
    navigator: { getGamepads: () => [] },
    performance: { now: () => Date.now() },
    requestAnimationFrame: () => 0,
    cancelAnimationFrame: () => {},
    // Timers are real (some app.js code paths schedule work through
    // them) but unref'd, so they never keep the process alive on their
    // own. Without this, app.js's WebRTC retry() loop -- which
    // re-schedules itself every 2s after each failed connection attempt
    // -- would spin forever and the suite would never exit.
    /* Real timers (some app.js paths genuinely need them) but unref'd so
     * they never keep the process alive, AND recorded so a test can fire
     * a deferred callback on demand. Without that, anything behind a
     * debounce -- the quality slider's POST, for one -- is unreachable
     * from a synchronous test suite. */
    setTimeout: (fn, ms) => {
      sandbox.pendingTimeouts.push(fn);
      const t = setTimeout(fn, ms);
      if (t && typeof t.unref === 'function') t.unref();
      return t;
    },
    clearTimeout,
    // app.js starts a 1s setInterval for its live stats display. Under
    // test there's nothing to poll and the callback would only fire
    // after the suite finished, so swallow the registration entirely.
    setInterval: () => 0,
    clearInterval: () => {},
    /* Still rejects, so nothing in app.js takes a "server answered"
     * path under test -- but the call is recorded first, which is what
     * lets a test assert that a player-gated endpoint carried its
     * session token. Forgetting that header is silent in the browser:
     * the server just answers 403. */
    fetch: (url, options) => {
      sandbox.fetchCalls.push({ url, options: options || {} });
      return Promise.reject(new Error('no network in test'));
    },
    Int8Array,
    Math,
    JSON,
    prompt: () => null,
    MediaStream: function () {
      return { addTrack() {} };
    },
    RTCPeerConnection: function () {
      return {
        createDataChannel: () => ({}),
        addTransceiver() {},
        getStats: () => Promise.resolve(new Map()),
        createOffer: () => Promise.resolve({ type: 'offer', sdp: '' }),
        setLocalDescription: () => Promise.resolve(),
        setRemoteDescription: () => Promise.resolve(),
        addEventListener() {},
        removeEventListener() {},
        iceGatheringState: 'complete'
      };
    }
  };
  sandbox.fetchCalls = [];
  sandbox.pendingTimeouts = [];
  /* Runs what is queued now. The real timer will also fire later; every
   * callback here is idempotent enough for that not to matter. */
  sandbox.runPendingTimers = () => {
    const queued = sandbox.pendingTimeouts.splice(0);
    queued.forEach((fn) => {
      try { fn(); } catch (e) {}
    });
  };
  sandbox.global = sandbox;

  vm.createContext(sandbox);
  vm.runInContext(src, sandbox, { filename: 'app.js' });
  return sandbox;
}

module.exports = { createSandbox };
