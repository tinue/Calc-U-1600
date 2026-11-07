const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

// The web client end to end through the real WASM engine, on dist/index.html
// (CalcU1600 with the physical keyboard). URLs are relative to the
// project's baseURL -- the server root or a sub-path (playwright.config.js).
// The display is read as text through the engine (Shared LcdText), not from
// pixels.

const FIXTURES = path.join(__dirname, 'fixtures');

async function gotoReady(page, query = '') {
  await page.goto(`./${query}`);
  await expect(page.locator('.calcu-client[data-ready=true]')).toHaveCount(1, { timeout: 30000 });
}

const request = (page, method, ...args) =>
  page.evaluate(([m, a]) => document.querySelector('.calcu-client').calcuEngine.request(m, ...a), [method, args]);

const screen = (page) => request(page, 'lcdText');

async function waitForScreen(page, re, timeout = 20000) {
  await expect.poll(() => screen(page), { timeout }).toMatch(re);
}

// A model from the control bar; done once the engine reports it.
async function pickModel(page, index) {
  await page.getByLabel('Model', { exact: true }).selectOption(String(index));
  await expect(page.locator(`.calcu-face[data-model="${index}"]`)).toHaveCount(1, { timeout: 30000 });
  await expect(page.locator('.calcu-busy')).toHaveCount(0, { timeout: 30000 });
}

// A window tall enough that the panels fit below the full-width faceplate.
const TALL = { width: 1280, height: 1000 };

async function menu(page, ...path) {
  await page.locator('.calcu-menubar-item > button', { hasText: path[0] }).click();
  for (const item of path.slice(1, -1)) await page.locator('.calcu-menu-item', { hasText: item }).last().hover();
  // The deepest match: an open item contains its submenu's text too.
  await page.locator('.calcu-menu-item', { hasText: path[path.length - 1] }).last().click();
}

test('boots, and each model comes up', async ({ page }) => {
  await gotoReady(page);
  await expect(page.locator('.calcu-face[data-model="1"]')).toHaveCount(1);  // PC-1500A, the default
  await waitForScreen(page, /NEW0\?/);
  await pickModel(page, 2);
  await waitForScreen(page, />/);
  await pickModel(page, 0);
  await expect(page.locator('.calcu-key')).not.toHaveCount(0);
  await expect(page.locator('[role=alertdialog]')).toHaveCount(0);
});

test('?model= and ?preset= start with them', async ({ page }) => {
  await gotoReady(page, '?model=2');
  await expect(page.locator('.calcu-face[data-model="2"]')).toHaveCount(1, { timeout: 30000 });
  await expect(page.locator('[role=alertdialog], [data-dialog=notice]')).toHaveCount(0);
  await gotoReady(page, '?model=0');
  await expect(page.locator('.calcu-face[data-model="0"]')).toHaveCount(1, { timeout: 30000 });
  await expect(page.locator('[data-dialog=notice]')).toHaveCount(0);
  await page.setViewportSize(TALL);
  await gotoReady(page, '?preset=plotter/lissajou-1600.pc1600');
  await expect(page.locator('[data-panel=paper][data-empty=false]')).toHaveCount(1, { timeout: 45000 });
  await expect(page.locator('[data-dialog=notice]')).toHaveCount(0);
});

// Settings > Default presets: the open list shows each bundled default by
// its description, the closed combo the chosen one's short name; a reload
// keeps the choice (stored as its examples: ref).
test('a default preset is picked by description and shown by name', async ({ page }) => {
  await gotoReady(page);
  await menu(page, 'Edit', 'Settings');
  const row = page.locator('[data-default-preset=PC1600]');
  const combo = row.getByRole('button', { name: 'PC-1600 default preset' });
  await expect(combo).toHaveText('None');
  await combo.click();
  await row.locator('.calcu-menu-item', { hasText: 'PC-1600 with the CE-1600P, a floppy and two modules' }).click();
  await expect(combo).toHaveText('CE-1600P + modules');
  expect((await request(page, 'settingsState')).defaultPresets.PC1600).toBe('examples:startup/default-pc1600.pc1600');
  // The examples don't offer the default presets.
  const examples = await request(page, 'presetExamples');
  expect(examples.some((e) => e.ref.startsWith('examples:startup/'))).toBe(false);
  expect(examples.find((e) => e.ref === 'examples:basic/hanoi.pc1600').name).toBe('Hanoi');
});

test('the panels take only the room a full-width faceplate leaves', async ({ page }) => {
  // A phone in landscape: the calculator alone, as large as it fits.
  await page.setViewportSize({ width: 844, height: 390 });
  await gotoReady(page);
  await expect(page.locator('.calcu-panels')).toHaveCount(0);
  const face = await page.locator('.calcu-face').boundingBox();
  expect(face.height).toBeGreaterThan(250);
  // In portrait the faceplate spans the width and the inspector fits below.
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.locator('[data-panel=inspector]')).toBeVisible();
  // A tall desktop window too; a wide, short one shows the calculator alone.
  await page.setViewportSize(TALL);
  await expect(page.locator('[data-panel=inspector]')).toBeVisible();
  await page.setViewportSize({ width: 1600, height: 700 });
  await expect(page.locator('.calcu-panels')).toHaveCount(0);
});

test('the client has the keyboard from the start, no click needed', async ({ page }) => {
  await gotoReady(page);
  await expect(page.locator('.calcu-client')).toBeFocused();
});

test('the physical keyboard types, ENTER runs it', async ({ page }) => {
  await gotoReady(page);
  await pickModel(page, 2);
  await page.locator('.calcu-client').focus();  // from the Model picker
  await page.keyboard.type('PRINT 6*7', { delay: 60 });
  await page.keyboard.press('Enter');
  await waitForScreen(page, /42/);
});

test('the keyboard comes back to the calculator after a picker or a click', async ({ page }) => {
  await gotoReady(page);
  // A picker change hands the keyboard back.
  await pickModel(page, 2);
  await page.keyboard.type('PRINT 6*7', { delay: 60 });
  await page.keyboard.press('Enter');
  await waitForScreen(page, /42/);
  // A button that had the focus doesn't get ENTER after a click on the
  // faceplate (off any key): the calculator does.
  await page.locator('.calcu-menubar-item > button', { hasText: 'Help' }).focus();
  const face = await page.locator('.calcu-face').boundingBox();
  await page.mouse.click(face.x + 4, face.y + 4);
  await page.keyboard.type('PRINT 7*8', { delay: 60 });
  await page.keyboard.press('Enter');
  await waitForScreen(page, /56/);
  await expect(page.locator('.calcu-menu')).toHaveCount(0);
});

test('a faceplate key types while its pointer is down', async ({ page }) => {
  await gotoReady(page);
  await pickModel(page, 2);
  const key = await page.locator('.calcu-key[aria-label="5"]').boundingBox();
  await page.mouse.move(key.x + key.width / 2, key.y + key.height / 2);
  await page.mouse.down();
  await expect(page.locator('.calcu-key[aria-label="5"]')).toHaveAttribute('aria-pressed', 'true');
  await page.waitForTimeout(150);
  await page.mouse.up();
  await expect(page.locator('.calcu-key[aria-label="5"]')).toHaveAttribute('aria-pressed', 'false');
  await waitForScreen(page, /5/);
});

test.describe('on a touchscreen', () => {
  test.use({ hasTouch: true });

  // An instant tap (down and up at once) in the gap just below a key: the
  // touch slop takes the nearest key, the engine's minimum hold lets the
  // ROM see it.
  test('a quick tap next to a key types it', async ({ page }) => {
    await gotoReady(page);
    await pickModel(page, 2);
    const key = await page.locator('.calcu-key[aria-label="5"]').boundingBox();
    await page.touchscreen.tap(key.x + key.width / 2, key.y + key.height + 3);
    await waitForScreen(page, /5/);
  });

  // The taller touch menu bar is counted: the control bar ends inside the
  // window (a phone in landscape, the calculator alone).
  test('the client fits the window with the touch menu bar', async ({ page }) => {
    await page.setViewportSize({ width: 932, height: 400 });
    await gotoReady(page, '?height=400');
    expect(await page.evaluate(() => matchMedia('(pointer: coarse)').matches)).toBe(true);
    const bar = await page.locator('.calcu-controlbar').boundingBox();
    const client = await page.locator('.calcu-client').boundingBox();
    expect(bar.y + bar.height).toBeLessThanOrEqual(client.y + client.height + 0.5);
  });
});

test('the control bar follows the model: peripherals, floppy, tape', async ({ page }) => {
  await page.setViewportSize(TALL);
  await gotoReady(page);
  // PC-1500A: CE-150 and CE-158 on the bar, no disk.
  await expect(page.getByRole('button', { name: 'CE-150' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'CE-158' })).toBeVisible();
  await expect(page.getByLabel('Disk', { exact: true })).toHaveCount(0);
  await page.getByRole('button', { name: 'CE-150' }).click();
  await expect(page.getByRole('button', { name: 'CE-150' })).toHaveAttribute('aria-pressed', 'true', { timeout: 30000 });
  await expect(page.getByLabel('Tape', { exact: true })).toBeVisible();
  await expect(page.locator('[data-panel=paper]')).toBeVisible();

  await pickModel(page, 2);
  await expect(page.getByRole('button', { name: 'CE-150' })).toHaveCount(0);
  await expect(page.getByLabel('Disk', { exact: true })).toBeDisabled();
  await page.getByRole('button', { name: 'CE-1600P' }).click();
  await expect(page.getByLabel('Disk', { exact: true })).toBeEnabled({ timeout: 30000 });
  // Machine ▸ Peripherals: the CE-150 is ruled out while the CE-1600P is on.
  await page.locator('.calcu-menubar-item > button', { hasText: 'Machine' }).click();
  await page.locator('.calcu-menu-item', { hasText: 'Peripherals' }).hover();
  await expect(page.locator('.calcu-submenu .calcu-menu-item', { hasText: 'CE-150' })).toHaveAttribute('aria-disabled', 'true');
  await expect(page.locator('.calcu-submenu .calcu-menu-item', { hasText: 'CE-1600P' })).toHaveAttribute('aria-checked', 'true');
});

test('a plotter preset draws on the paper', async ({ page }) => {
  await page.setViewportSize(TALL);
  await gotoReady(page);
  await menu(page, 'File', 'Load Preset', 'Lissajous figure on the CE-1600P');
  await expect(page.locator('[data-panel=paper][data-empty=false]')).toHaveCount(1, { timeout: 45000 });
  await expect(page.locator('[data-panel=paper] .calcu-panel-header')).toHaveText('CE-1600P');
});

test('Name & Save keeps a card across a reload', async ({ page }) => {
  await gotoReady(page);
  await pickModel(page, 2);
  await page.getByLabel('Slot 1', { exact: true }).selectOption('CE-1600M');
  await expect(page.getByRole('button', { name: 'Name & Save slot 1' })).toBeEnabled({ timeout: 30000 });
  await page.getByRole('button', { name: 'Name & Save slot 1' }).click();
  const name = `Work ${Date.now()}`;
  await page.locator('[data-dialog=prompt] input').fill(name);
  await page.locator('[data-dialog=prompt] button', { hasText: 'OK' }).click();
  await expect(page.getByLabel('Slot 1', { exact: true })).toHaveValue(name);
  await page.waitForTimeout(500);  // IndexedDB write

  await page.reload();
  await expect(page.locator('.calcu-client[data-ready=true]')).toHaveCount(1, { timeout: 30000 });
  await expect(page.locator('.calcu-face[data-model="2"]')).toHaveCount(1);  // last used model
  await expect(page.getByLabel('Slot 1', { exact: true }).locator('option', { hasText: name })).toHaveCount(1);

  // Saved Media lists it, in use it can't be deleted; out of the slot it can.
  await page.getByLabel('Slot 1', { exact: true }).selectOption(name);
  await expect(page.locator('.calcu-busy')).toHaveCount(0, { timeout: 30000 });
  await menu(page, 'File', 'Saved Media…');
  const row = page.locator(`[data-media="${name}"]`);
  await expect(row).toContainText('in use');
  await expect(row.getByRole('button', { name: 'Delete' })).toBeDisabled();
  await page.locator('[data-dialog=media] button', { hasText: 'Close' }).click();
  await page.getByLabel('Slot 1', { exact: true }).selectOption('');
  await expect(page.locator('.calcu-busy')).toHaveCount(0, { timeout: 30000 });
  await menu(page, 'File', 'Saved Media…');
  await row.getByRole('button', { name: 'Delete' }).click();
  await expect(row).toHaveCount(0);
});

test('a tape imported as WAV shows in the tape picker', async ({ page }) => {
  await gotoReady(page);
  const chooser = page.waitForEvent('filechooser');
  await menu(page, 'File', 'Import Card, Disk or Tape…');
  const name = `Side ${Date.now()}`;
  await (await chooser).setFiles({ name: `${name}.wav`, mimeType: 'audio/wav', buffer: fs.readFileSync(path.join(FIXTURES, 'pc1500_tape.wav')) });
  await page.getByRole('button', { name: 'CE-150' }).click();
  await expect(page.getByLabel('Tape', { exact: true }).locator('option', { hasText: name })).toHaveCount(1, { timeout: 30000 });
});

test('Load Machine Code asks for the address, then tells NEW and CALL', async ({ page }) => {
  await gotoReady(page);
  await pickModel(page, 2);
  const chooser = page.waitForEvent('filechooser');
  await menu(page, 'File', 'Load Machine Code…');
  await (await chooser).setFiles(path.join(FIXTURES, 'rtn4.bin'));
  const dialog = page.locator('[data-dialog=machinecode]');
  await expect(dialog.locator('input')).toHaveValue('&C0C5');
  await dialog.locator('input').fill('zz');
  await expect(dialog).toContainText('Not a hex address');
  await expect(dialog.getByRole('button', { name: 'Load' })).toBeDisabled();
  await dialog.locator('input').fill('&C0C5');
  await dialog.getByRole('button', { name: 'Load' }).click();
  const loaded = page.locator('[data-dialog=loaded]');
  await expect(loaded).toContainText('Loaded 4 bytes at &C0C5');
  await expect(loaded).toContainText('CALL &C0C5');
  await loaded.getByRole('button', { name: 'OK' }).click();
  await waitForScreen(page, /CALL &C0C5/);
});

test('a WAV with several programs asks which one', async ({ page }) => {
  await gotoReady(page);
  // The fixture tape twice in one WAV: its data chunk doubled.
  const wav = fs.readFileSync(path.join(FIXTURES, 'pc1500_tape.wav'));
  let at = 12;
  while (wav.toString('ascii', at, at + 4) !== 'data') at += 8 + wav.readUInt32LE(at + 4);
  const size = wav.readUInt32LE(at + 4);
  const data = wav.subarray(at + 8, at + 8 + size);
  const doubled = Buffer.concat([wav.subarray(0, at + 8 + size), data]);
  doubled.writeUInt32LE(size * 2, at + 4);
  doubled.writeUInt32LE(doubled.length - 8, 4);

  const chooser = page.waitForEvent('filechooser');
  await menu(page, 'File', 'Load BASIC Program…');
  await (await chooser).setFiles({ name: 'two.wav', mimeType: 'audio/wav', buffer: doubled });
  const dialog = page.locator('[data-dialog=choice]');
  await expect(dialog).toContainText('two.wav holds 2 files. Load which one?');
  await expect(dialog.locator('option')).toHaveCount(2);
  await dialog.getByRole('button', { name: 'OK' }).click();
  await expect(dialog).toHaveCount(0);
});

test('the inspector shows the pointers and the card dump', async ({ page }) => {
  await page.setViewportSize(TALL);
  await gotoReady(page);
  await page.locator('[data-panel=inspector] button', { hasText: 'Pointers' }).click();
  await expect(page.locator('.calcu-inspector-output')).toContainText('RAM_ST');
  await page.locator('[data-panel=inspector] button', { hasText: 'Card YAML' }).click();
  await expect(page.locator('.calcu-inspector-output')).toContainText('Module card dump');
});

test('a second tab says its changes are not saved', async ({ page, context }) => {
  await gotoReady(page);
  await expect(page.locator('.calcu-banner')).toHaveCount(0);
  const second = await context.newPage();
  await gotoReady(second);
  await expect(second.locator('.calcu-banner')).toContainText('open in another tab');
});

test('the installed app shell starts', async ({ page }) => {
  await page.goto('./app/');
  await expect(page.locator('.calcu-client[data-ready=true]')).toHaveCount(1, { timeout: 30000 });
  await expect(page.locator('.calcu-menubar')).toBeVisible();
});
