// Background Worker of the web client (src/engine.js starts it). It owns the
// WASM WebController (Web/bindings.cpp) -- the real shared::AppSession, run
// single-threaded -- and calls its 60 Hz tick. Everything that isn't
// drawing happens in there; this file only fetches files, mounts storage
// and passes messages. Every URL is relative to dist/ (this script, or
// CALC_ROOT when started through app/engine-worker.js), so the client works
// wherever it is hosted.
//
// File system (Emscripten):
//   /bundle   ROMs and the bundled cards / disks (files.json "bundle")
//   /presets  the bundled example presets with their files ("presets")
//   /uploads  files the user opened
//   /persist  the save folder (cards, disks, tapes): IndexedDB (IDBFS).
//             One tab at a time writes it (Web Locks); another tab gets a
//             copy that isn't saved, and says so.
//
// Protocol, main thread -> worker:
//   {type: "init", settings}            first: stored settings by key
//   {type: "call", method, args, id}    a WebController method; with an id,
//                                       its result comes back as reply
//   {type: "pause"} / {type: "resume"}  document.visibilitychange
// worker -> main thread:
//   {type: "ready", faceplates, lcd, persistent}
//   {type: "reply", id, value} / {type: "replyError", id, message}
//   {type: "fatal", message}
//   and what WebController posts itself (bindings.cpp): ui, display, lamps,
//   paper, printer, busy, notice, log, store, audio.

const ROOT = self.CALC_ROOT || "";
const WASM_DIR = `${ROOT}wasm/`;
importScripts(`${WASM_DIR}calcu1600-core.js`);

const FRAME_MS = 1000 / 60;
const LOCK_NAME = "calcu1600-storage";

let Module = null;
let controller = null;
let intervalId = null;
const pending = [];

function fatal(message) {
  stopLoop();
  postMessage({ type: "fatal", message });
}

function errorText(e) {
  if (Module && typeof WebAssembly !== "undefined" && WebAssembly.Exception && e instanceof WebAssembly.Exception) {
    try {
      const [type, message] = Module.getExceptionMessage(e);
      return message || type;
    } catch (_) { /* fall through */ }
  }
  return String((e && e.message) || e);
}

function tick() {
  try {
    controller.tick();
  } catch (e) {
    fatal(errorText(e));
  }
}

function startLoop() {
  if (intervalId === null && controller) intervalId = setInterval(tick, FRAME_MS);
}

function stopLoop() {
  if (intervalId === null) return;
  clearInterval(intervalId);
  intervalId = null;
}

function handle(msg) {
  switch (msg.type) {
    case "call": {
      let value;
      try {
        value = controller[msg.method](...(msg.args || []));
      } catch (e) {
        if (msg.id) postMessage({ type: "replyError", id: msg.id, message: errorText(e) });
        else postMessage({ type: "notice", title: "Error", text: errorText(e) });
        return;
      }
      if (msg.id) postMessage({ type: "reply", id: msg.id, value });
      break;
    }
    case "pause": stopLoop(); break;
    case "resume": startLoop(); break;
    default: break;
  }
}

async function fetchBytes(path) {
  const r = await fetch(`${ROOT}${path}`);
  if (!r.ok) throw new Error(`${path}: ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

function writeTree(FS, root, files) {
  for (const [rel, bytes] of files) {
    const parts = `${root}/${rel}`.split("/").filter(Boolean);
    let dir = "";
    for (const part of parts.slice(0, -1)) {
      dir += `/${part}`;
      try { FS.mkdir(dir); } catch (_) { /* exists */ }
    }
    FS.writeFile(`/${parts.join("/")}`, bytes);
  }
}

// Whether this tab may write the shared storage: it holds the lock for the
// rest of its life, or another tab already does.
function acquireStorageLock() {
  if (!(self.navigator && navigator.locks)) return Promise.resolve(true);
  return new Promise((resolve) => {
    navigator.locks.request(LOCK_NAME, { ifAvailable: true }, (lock) => {
      resolve(!!lock);
      return lock ? new Promise(() => {}) : undefined;  // keep it
    });
  });
}

function syncFromStorage(FS) {
  return new Promise((resolve) => FS.syncfs(true, (err) => resolve(err)));
}

async function init({ settings }) {
  const manifest = JSON.parse(new TextDecoder().decode(await fetchBytes("files.json")));
  const fetchAll = (dir, names) => Promise.all(names.map(async (n) => [n, await fetchBytes(`${dir}/${n}`)]));
  const [mod, bundle, presets, persistent] = await Promise.all([
    createCalcU1600Module({ locateFile: (path) => `${WASM_DIR}${path}` }),
    fetchAll("bundle", manifest.bundle),
    fetchAll("presets", manifest.presets),
    acquireStorageLock(),
  ]);
  Module = mod;
  const FS = Module.FS;
  writeTree(FS, "/bundle", bundle);
  writeTree(FS, "/presets", presets);
  FS.mkdir("/uploads");
  FS.mkdir("/persist");
  // autoPersist: every write, rename or delete under /persist goes to
  // IndexedDB on its own. A tab without the lock reads the storage once and
  // keeps its changes in memory.
  FS.mount(Module.IDBFS, { autoPersist: persistent }, "/persist");
  const err = await syncFromStorage(FS);
  if (err) postMessage({ type: "log", text: `Saved files could not be read: ${err}` });

  postMessage({
    type: "ready",
    persistent,
    lcd: Module.WebController.lcdConstants(),
    faceplates: [0, 1, 2].map((m) => Module.WebController.faceplate(m)),
  });
  controller = new Module.WebController(settings);
  try {
    controller.start();
  } catch (e) {
    fatal(errorText(e));
    return;
  }
  startLoop();
  while (pending.length) handle(pending.shift());
}

onmessage = (e) => {
  if (e.data.type === "init") {
    init(e.data).catch((err) => fatal(errorText(err)));
  } else if (!controller) {
    pending.push(e.data);
  } else {
    handle(e.data);
  }
};
