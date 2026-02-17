const CACHE_NAME = "scalex-rally-v3";
const ASSETS = [
  "./assets/home/quick.png",
  "./assets/home/create.png",
  "./assets/home/pilots.png",
  "./assets/home/tracks.png",
  "./assets/home/history.png",
  "./assets/icons/icon-192.svg",
  "./assets/icons/icon-512.svg"
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(ASSETS)).then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) => Promise.all(keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k)))).then(() => self.clients.claim())
  );
});

self.addEventListener("message", (event) => {
  if (event.data === "SKIP_WAITING") {
    self.skipWaiting();
  }
});

self.addEventListener("fetch", (event) => {
  if (event.request.method !== "GET") return;

  const url = new URL(event.request.url);

  // Always prefer fresh HTML/navigation to avoid stale UI.
  if (event.request.mode === "navigate" || url.pathname.endsWith("/index.html")) {
    event.respondWith(
      fetch(event.request, { cache: "no-store" }).catch(() => caches.match("./index.html"))
    );
    return;
  }

  // Keep manifest and worker fresh as well.
  if (url.pathname.endsWith("/manifest.webmanifest") || url.pathname.endsWith("/sw.js")) {
    event.respondWith(fetch(event.request, { cache: "no-store" }).catch(() => caches.match(event.request)));
    return;
  }

  event.respondWith(
    caches.match(event.request).then((cached) => {
      if (cached) return cached;
      return fetch(event.request)
        .then((res) => {
          const copy = res.clone();
          caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
          return res;
        })
        .catch(() => caches.match("./index.html"));
    })
  );
});
