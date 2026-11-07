const { defineConfig, devices } = require('@playwright/test');

// run-tests.sh serves dist/ at a server root and, on the next port, only
// under a sub-path: the client must work unchanged wherever it is hosted
// (docs/developer/Web-Client.md). Every play test runs against each.
const PORT = Number(process.env.CALCU_WEB_PORT || 8160);

module.exports = defineConfig({
  testDir: './tests',
  timeout: 60000,
  workers: 2,
  use: { headless: true },
  projects: [
    { name: 'root', testMatch: /play|prerender/, use: { ...devices['Desktop Chrome'], baseURL: `http://localhost:${PORT}/` } },
    { name: 'subpath', testMatch: /play/, use: { ...devices['Desktop Chrome'], baseURL: `http://localhost:${PORT + 1}/anything/calc/` } },
    // The offline start brings its own server, in both engines: an iPad's
    // installed app is WebKit.
    { name: 'offline-chromium', testMatch: /offline/, use: { ...devices['Desktop Chrome'] } },
    { name: 'offline-webkit', testMatch: /offline/, use: { ...devices['Desktop Safari'] } },
  ],
});
