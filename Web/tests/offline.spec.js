const { test, expect } = require('@playwright/test');
const fs = require('fs');
const http = require('http');
const path = require('path');

// The installed app started without a connection: a new page after the
// server is gone, so the worker, its scripts, the WASM, the ROMs and the
// presets can only come from the service worker. Its own server (no-store),
// since with setOffline() the HTTP cache would still answer. Also in
// WebKit, the engine of an iPad's installed app (playwright.config.js).

const DIST = path.join(__dirname, '..', 'dist');
const PREFIX = '/anything/calc/';
const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.wasm': 'application/wasm', '.png': 'image/png',
};

function serveDist() {
  const server = http.createServer((req, res) => {
    let url = decodeURIComponent(req.url.split('?')[0]);
    if (url.endsWith('/')) url += 'index.html';
    const file = path.join(DIST, url.slice(PREFIX.length));
    if (!url.startsWith(PREFIX) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      res.writeHead(404);
      res.end();
      return;
    }
    res.writeHead(200, { 'content-type': TYPES[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, () => resolve(server)));
}

function stop(server) {
  server.closeAllConnections();
  return new Promise((resolve) => server.close(resolve));
}

test('the installed app starts without a connection and runs every model', async ({ page, context }) => {
  test.setTimeout(120000);
  const server = await serveDist();
  const appUrl = `http://localhost:${server.address().port}${PREFIX}app/`;

  await page.goto(appUrl);
  await expect(page.locator('.calcu-client[data-ready=true]')).toHaveCount(1, { timeout: 30000 });
  await page.evaluate(() => navigator.serviceWorker.ready);
  await page.close();
  await stop(server);

  const app = await context.newPage();
  await app.goto(appUrl);
  await expect(app.locator('.calcu-client[data-ready=true]')).toHaveCount(1, { timeout: 30000 });
  for (const model of ['2', '0', '1']) {
    await app.getByLabel('Model', { exact: true }).selectOption(model);
    await expect(app.locator(`.calcu-face[data-model="${model}"]`)).toHaveCount(1, { timeout: 30000 });
  }
  // A bundled preset, with its program file, from the cache.
  await app.locator('.calcu-menubar-item > button', { hasText: 'File' }).click();
  await app.locator('.calcu-menu-item', { hasText: 'Load Preset' }).hover();
  await app.locator('.calcu-submenu .calcu-menu-item', { hasText: 'Towers of Hanoi' }).click();
  await expect(app.locator('.calcu-face[data-model="2"]')).toHaveCount(1, { timeout: 30000 });
  await expect(app.locator('[role=alertdialog], [data-dialog=notice]')).toHaveCount(0);
});
