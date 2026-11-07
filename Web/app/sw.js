// Service worker for the installed Calc-U-1600 app (scope: this directory).
// Precaches the app shell and everything the emulator needs offline -- the
// WASM engine, the ROMs and bundled cards/disks, the faceplates, the
// presets -- so the installed app opens and runs without a network after
// the first launch.
//
// Every path is relative to this script. Web/build.js fills in the
// generated list, checks every file exists in dist/ and stamps CACHE_NAME
// with a hash of their contents; activate drops this app's older caches.
// The scope is part of the name: the origin may be shared with the host
// site or another copy of the client.
const CACHE_PREFIX = `calcu1600-app ${self.registration.scope} `;
const CACHE_NAME = CACHE_PREFIX + "__CACHE_VERSION__";

const CORE_ASSETS = [
  "./",
  "index.html",
  "app.css",
  "app.js",
  "engine-worker.js",
  "manifest.json",
  "icon.png",

  "../vendor/react.production.min.js",
  "../vendor/react-dom.production.min.js",

  "../calc.js",
  "../calc.css",
  "../calc-engine-worker.js",
  "../audio-worklet.js",
  "../files.json",

  "../img/pc1500.png",
  "../img/pc1500A.png",
  "../img/pc1600.png",
  "../img/icon.png",

  "../wasm/calcu1600-core.js",
  "../wasm/calcu1600-core.wasm",
  // Every file in bundle/ and presets/; Web/build.js fills them in.
  /*__GENERATED_ASSETS__*/
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(CORE_ASSETS))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(
        keys.filter((key) => key.startsWith(CACHE_PREFIX) && key !== CACHE_NAME).map((key) => caches.delete(key))
      ))
      .then(() => self.clients.claim())
  );
});

// Cache-first: the files are versioned by CACHE_NAME, not by URL.
self.addEventListener("fetch", (event) => {
  if (event.request.method !== "GET") return;
  event.respondWith(caches.match(event.request, { ignoreSearch: true }).then((cached) => cached || fetch(event.request)));
});
