const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

// A host site may prerender CalcU1600 server-side, in a sandbox whose
// fetch() and Worker throw: rendering must have no side effects -- those
// belong in effects, which a static render never runs. This renders
// dist/calc.js the same way, in Node.

const WEB = path.join(__dirname, '..');

function sandbox() {
  const noop = () => {};
  const ctx = {
    console, URL, Math, Date, JSON, Uint8Array, TextEncoder, TextDecoder, setTimeout, clearTimeout, queueMicrotask,
    location: { href: 'https://example.org/somewhere/', pathname: '/somewhere/' },
    navigator: { userAgent: 'prerender', platform: 'prerender' },
    fetch: () => { throw new Error('fetch() during render'); },
    Worker: function () { throw new Error('new Worker() during render'); },
    document: {
      currentScript: null,
      addEventListener: noop, removeEventListener: noop,
      createElement: () => ({ style: {} }), getElementById: () => null,
    },
  };
  ctx.window = ctx;
  ctx.self = ctx;
  return vm.createContext(ctx);
}

test('CalcU1600 renders server-side without fetch or Worker', async () => {
  const ctx = sandbox();
  const run = (file) => vm.runInContext(fs.readFileSync(file, 'utf8'), ctx, { filename: file });
  run(path.join(WEB, 'vendor', 'react.production.min.js'));
  run(path.join(WEB, 'vendor', 'react-dom-server-legacy.browser.production.min.js'));
  run(path.join(WEB, 'dist', 'calc.js'));

  expect(Object.keys(ctx).filter((k) => /^(createEngine|useEngine|Faceplate|ControlBar|MenuBar|Inspector|paintLcd)$/.test(k)),
         'calc.js leaks its internals into the global scope').toEqual([]);
  const html = ctx.ReactDOMServer.renderToStaticMarkup(ctx.React.createElement(ctx.CalcU1600, { width: 1000, height: 700 }));
  expect(html).toContain('calcu-client');
  expect(html).toContain('calcu-menubar');
  expect(html).toContain('calcu-face');
});
