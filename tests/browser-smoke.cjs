// Run after web-smoke.cjs + prepare_archives.py. Uses a project subpath to
// catch asset URLs that work at localhost root but fail on GitHub Pages.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');
const root = path.resolve(__dirname, '..');
const fixtures = path.join(root, 'build-web/test-fixtures');
const types = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((request, response) => {
  let relative = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
  if (!relative.startsWith('/spc2vgm/')) { response.writeHead(404).end(); return; }
  relative = relative.slice('/spc2vgm/'.length) || 'index.html';
  const filename = path.resolve(root, 'web', relative);
  if (!filename.startsWith(path.join(root, 'web') + path.sep)) { response.writeHead(403).end(); return; }
  fs.readFile(filename, (error, bytes) => {
    if (error) response.writeHead(404).end();
    else response.writeHead(200, { 'Content-Type': types[path.extname(filename)] || 'text/plain' }).end(bytes);
  });
});
(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || undefined, args: ['--no-sandbox', '--disable-dev-shm-usage'] });
    const page = await browser.newPage({ viewport: { width: 1280, height: 1100 }, acceptDownloads: true });
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    const requests = []; page.on('request', request => requests.push(request));
    await page.goto(`http://127.0.0.1:${server.address().port}/spc2vgm/`);
    await page.screenshot({ path: path.join(fixtures, 'browser-desktop.png'), fullPage: true });
    await page.locator('#files').setInputFiles(path.join(fixtures, 'soundtrack.rsn'));
    await page.locator('#duration').selectOption('10');
    await page.locator('#convert').click();
    await page.locator('#download').waitFor({ state: 'visible', timeout: 120000 });
    assert.match(await page.locator('#status').textContent(), /1 track converted/);
    const downloadEvent = page.waitForEvent('download'); await page.locator('#download').click();
    const download = await downloadEvent; assert.equal(download.suggestedFilename(), 'soundtrack-vgm.zip');
    await download.saveAs(path.join(fixtures, 'browser-output.zip'));
    console.log('Browser: RSN -> extraction -> conversion -> optimizer -> downloadable ZIP');
    // Cancel an active conversion, then start another to prove recovery.
    await page.locator('#duration').selectOption('180');
    await page.locator('#convert').click();
    await page.waitForFunction(() => /Converting 1 of/.test(document.getElementById('status').textContent));
    await page.locator('#cancel').click();
    assert.match(await page.locator('#status').textContent(), /Cancelled/);
    assert.equal(await page.locator('#convert').isEnabled(), true);
    await page.locator('#duration').selectOption('10');
    await page.locator('#files').setInputFiles([path.join(fixtures, 'tone.spc'), path.join(fixtures, 'invalid.spc')]);
    await page.locator('#optimize').uncheck(); await page.locator('#convert').click();
    await page.locator('#download').waitFor({ state: 'visible', timeout: 120000 });
    assert.match(await page.locator('#status').textContent(), /1 track converted; 1 failed/);
    console.log('Browser: cancellation, restart, invalid track and partial results');
    await page.locator('#files').setInputFiles(path.join(fixtures, 'invalid.rsn'));
    await page.locator('#convert').click();
    await page.waitForFunction(() => !document.getElementById('convert').disabled);
    assert.equal(await page.locator('#download').isVisible(), false);
    assert.match(await page.locator('#status').textContent(), /Unsupported|invalid|archive/i);
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({ path: path.join(fixtures, 'browser-mobile.png'), fullPage: true });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
    assert.deepEqual(errors, []);
    assert.ok(requests.every(request => request.method() === 'GET' && new URL(request.url()).hostname === '127.0.0.1'));
    console.log('Browser: mobile layout, no page errors, no uploads or external requests');
  } finally { await browser?.close(); await new Promise(resolve => server.close(resolve)); }
})().catch(error => { console.error(error); process.exitCode = 1; });
