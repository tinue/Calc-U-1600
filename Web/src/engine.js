// The engine: owns calc-engine-worker.js (the WASM AppSession) and keeps
// what it posts in a small store the components subscribe to -- the web's
// Qt6/app/MachineController. LCD frames bypass the store: the LCD canvas
// repaints from `frames`, once per animation frame, without a React render.
// Settings the worker asks to keep go to localStorage; the buzzer plays
// through an AudioWorklet. Created in an effect only (a host page may
// prerender without a Worker).

// The directory calc.js was loaded from: the worker, the engine, the
// artwork and the presets sit beside it, wherever the client is hosted.
// Captured while the script runs (document.currentScript is null
// afterwards); without one -- a server-side prerender -- relative to the
// page, which a render never uses.
const CALC_BASE = (() => {
  const script = typeof document !== "undefined" ? document.currentScript : null;
  if (script && script.src) return new URL(".", script.src).href;
  return typeof location !== "undefined" ? new URL(".", location.href).href : "";
})();

const STORAGE_PREFIX = "calcu1600/";

function storageSet(key, value) {
  try {
    if (value === null) window.localStorage.removeItem(STORAGE_PREFIX + key);
    else window.localStorage.setItem(STORAGE_PREFIX + key, value);
  } catch (e) { /* private mode */ }
}

function storedSettings() {
  const out = {};
  try {
    const ls = window.localStorage;
    for (let i = 0; i < ls.length; i++) {
      const k = ls.key(i);
      if (k && k.startsWith(STORAGE_PREFIX)) out[k.slice(STORAGE_PREFIX.length)] = ls.getItem(k);
    }
  } catch (e) { /* private mode: defaults */ }
  return out;
}

function downloadBytes(name, bytes, type) {
  const url = URL.createObjectURL(new Blob([bytes], { type: type || "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ── Buzzer audio ───────────────────────────────────────────────────────
// The worker sends PCM as the emulation makes it; an AudioWorklet plays it
// from a ring buffer. Browsers only start audio from a user gesture, so the
// context is created on the first key or click (unlock()); until then the
// samples are dropped.
function createAudio() {
  let context = null;
  let node = null;
  let rate = 48000;
  let starting = null;

  const unlock = () => {
    if (context || starting || typeof AudioContext === "undefined") {
      if (context && context.state === "suspended") context.resume();
      return;
    }
    const ctx = new AudioContext({ latencyHint: "interactive" });
    starting = ctx.audioWorklet.addModule(`${CALC_BASE}audio-worklet.js`).then(() => {
      node = new AudioWorkletNode(ctx, "calcu-buzzer", { outputChannelCount: [1] });
      node.connect(ctx.destination);
      node.port.postMessage({ rate });
      context = ctx;
    }).catch(() => { /* no audio */ }).finally(() => { starting = null; });
  };

  const message = (m) => {
    if (m.op === "start") {
      rate = m.rate;
      if (node) node.port.postMessage({ rate });
    } else if (m.op === "data" && node) {
      node.port.postMessage({ pcm: m.pcm }, [m.pcm.buffer]);
    }
  };

  const close = () => {
    if (context) context.close();
    context = null;
    node = null;
  };

  return { unlock, message, close };
}

const INITIAL_STATE = {
  ready: false,
  fatal: "",
  persistent: true,
  faceplates: null,   // per model: {width, height, lcd, keys} in art pixels
  lcd: null,          // LcdLayout constants and colours
  ui: null,           // control state, pickers (bindings.cpp postUi())
  lamps: { floppyMotor: false, tapeMode: "", tapeMotor: false, tapeSeconds: 0, tapeFile: "" },
  busy: false,
  notices: [],        // [{title, text}], shown one at a time
  paper: null,        // {kind, empty, contentHeight, strokes}
  printer: { text: "", chunks: [] },
  log: [],            // preset step lines, for the inspector
};

// model (0-2) and preset (a path under presets/) start the engine with them.
function createEngine({ workerUrl, model, preset }) {
  let state = INITIAL_STATE;
  const listeners = new Set();
  const frames = { display: null, onFrame: null };
  let framePending = false;
  const replyWaiters = new Map();
  let lastRequest = 0;
  const audio = createAudio();

  const set = (patch) => {
    state = { ...state, ...patch };
    listeners.forEach((l) => l());
  };

  const worker = new Worker(workerUrl || `${CALC_BASE}calc-engine-worker.js`);
  const post = (msg, transfer) => worker.postMessage(msg, transfer || []);

  worker.onmessage = (e) => {
    const m = e.data;
    switch (m.type) {
      case "ready":
        set({ ready: true, faceplates: m.faceplates, lcd: m.lcd, persistent: m.persistent });
        break;
      case "display":
        frames.display = m;
        if (!framePending) {
          framePending = true;
          requestAnimationFrame(() => {
            framePending = false;
            if (frames.onFrame) frames.onFrame();
          });
        }
        break;
      case "ui": set({ ui: m }); break;
      case "lamps": set({ lamps: m }); break;
      case "busy": set({ busy: m.busy }); break;
      case "paper": set({ paper: m }); break;
      case "printer":
        set({ printer: { text: state.printer.text + m.text, chunks: state.printer.chunks.concat([m.bytes]) } });
        break;
      case "notice": set({ notices: state.notices.concat([{ title: m.title, text: m.text }]) }); break;
      case "log": set({ log: state.log.concat([m.text]).slice(-500) }); break;
      case "store": storageSet(m.key, m.value); break;
      case "audio": audio.message(m); break;
      case "fatal": set({ fatal: m.message }); break;
      case "reply": {
        const w = replyWaiters.get(m.id);
        replyWaiters.delete(m.id);
        if (w) w.resolve(m.value);
        break;
      }
      case "replyError": {
        const w = replyWaiters.get(m.id);
        replyWaiters.delete(m.id);
        if (w) w.reject(new Error(m.message));
        break;
      }
      default: break;
    }
  };
  worker.onerror = (e) => set({ fatal: e.message || "The emulator could not start." });

  post({
    type: "init",
    settings: storedSettings(),
    model: Number.isInteger(Number(model)) && model !== null && model !== "" ? Number(model) : -1,
    preset: preset ? `/presets/${preset}` : "",
  });

  const onVisibility = () => post({ type: document.hidden ? "pause" : "resume" });
  document.addEventListener("visibilitychange", onVisibility);

  return {
    frames,
    getState: () => state,
    subscribe: (l) => { listeners.add(l); return () => listeners.delete(l); },
    /// A WebController method (bindings.cpp), result not wanted.
    call: (method, ...args) => post({ type: "call", method, args }),
    /// A WebController method whose result is wanted.
    request: (method, ...args) => new Promise((resolve, reject) => {
      const id = ++lastRequest;
      replyWaiters.set(id, { resolve, reject });
      post({ type: "call", method, args, id });
    }),
    unlockAudio: audio.unlock,
    dismissNotice: () => set({ notices: state.notices.slice(1) }),
    clearPrinter: () => set({ printer: { text: "", chunks: [] } }),
    destroy: () => {
      document.removeEventListener("visibilitychange", onVisibility);
      worker.terminate();
      audio.close();
    },
  };
}

// The engine's state for a component: a slice of it.
const NO_ENGINE = { getState: () => INITIAL_STATE, subscribe: () => () => {} };
function useEngine(engine, select) {
  const e = engine || NO_ENGINE;
  return React.useSyncExternalStore(e.subscribe, () => select(e.getState()), () => select(INITIAL_STATE));
}

// A file next to calc.js, as text.
function fetchBesideCalc(path) {
  return fetch(`${CALC_BASE}${path}`).then((r) => {
    if (!r.ok) throw new Error(`${path}: ${r.status}`);
    return r.text();
  });
}

// A picked/dropped File's bytes.
function fileBytes(file) {
  return file.arrayBuffer().then((b) => new Uint8Array(b));
}
