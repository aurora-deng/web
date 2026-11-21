const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { setTimeout: wait } = require('node:timers/promises');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

async function test() {
  const root = path.resolve(__dirname, '../..');
  const output = path.join(root, 'build-function3');
  fs.mkdirSync(output, { recursive: true });
  const data = fs.mkdtempSync(path.join(output, 'yantu-browser-'));
  const reservation = net.createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const base = 'http://127.0.0.1:' + port;
  const service = spawn('python', ['-B', '-m', 'client.yantu.LocalWorkspace.LocalWorkspace',
    '--data-dir', data, '--port', String(port)], { cwd: root, windowsHide: true, stdio: 'ignore' });
  const exited = new Promise(resolve => service.once('exit', resolve));
  const post = (route, token) => fetch(base + route, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: 'Bearer ' + token } : {}) },
    body: '{}',
  });
  let browser;
  try {
    let ready = false;
    for (let attempt = 0; attempt < 50 && service.exitCode === null; ++attempt) {
      try { if ((await fetch(base + '/api/health')).ok) { ready = true; break; } } catch (_) {}
      await wait(100);
    }
    assert(ready, 'fresh Yantu process did not start');
    browser = await chromium.launch({ headless: true, executablePath: process.env.CHAT_TEST_BROWSER });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    // Exercise the browser file-system handle in an origin-private fixture folder.
    await page.addInitScript(() => {
      window.showDirectoryPicker = async () =>
        (await navigator.storage.getDirectory()).getDirectoryHandle('test-downloads', { create: true });
    });
    await page.goto(base + '/#download-settings');
    await page.waitForSelector('#download-settings[open]');
    await page.locator('[data-pick-folder]').click();
    await page.waitForFunction(() => document.querySelector('#download-settings [role=status]')
      .textContent.includes('test-downloads'));
    await page.locator('#download-settings button[type=submit]').click();
    await page.evaluate(() => NebulaDownloads.blob('fixture.txt', new Blob(['synthetic only'])));
    await page.evaluate(() => NebulaDownloads.url('app-manifest.json', '/manifest.webmanifest'));
    const downloads = await page.evaluate(async () => {
      const directory = await (await navigator.storage.getDirectory()).getDirectoryHandle('test-downloads');
      const read = async name => (await (await directory.getFileHandle(name)).getFile()).text();
      return { content: await read('fixture.txt'), manifest: JSON.parse(await read('app-manifest.json')) };
    });
    assert.equal(downloads.content, 'synthetic only');
    assert.match(downloads.manifest.name, /NEBULA/);
    const pdfVersion = await page.evaluate(async () => {
      const pdf = await import('/vendor/pdfjs-5.6.205/build/pdf.min.mjs');
      const worker = await fetch('/vendor/pdfjs-5.6.205/build/pdf.worker.min.mjs');
      if (!worker.ok) throw new Error('Missing PDF worker');
      return pdf.version;
    });
    assert.equal(pdfVersion, '5.6.205');
    await page.waitForFunction(() => Boolean(window.PrepLocal && window.DeviceStore));
    const empty = await page.evaluate(() => [state.timelines.length, state.schools.length, state.notes.length]);
    assert.deepEqual(empty, [0, 0, 0]);
    for (const view of ['timeline', 'mistakes', 'resources', 'mindmaps', 'papers', 'scores',
      'review', 'schools', 'exams', 'connection', 'notifications', 'mood', 'assistant']) {
      await page.locator('.nav-item[data-view="' + view + '"]').click();
      assert(await page.locator('.nav-item[data-view="' + view + '"]').evaluate(node =>
        node.classList.contains('active')));
    }
    assert.deepEqual(errors, []);
    console.log('PASS Yantu workspace tabs, empty startup, PDF assets and local download target');
  } finally {
    await browser?.close();
    try {
      const { deviceToken } = await (await post('/api/pair')).json();
      await post('/api/shutdown', deviceToken);
    } catch (_) {}
    await Promise.race([exited, wait(3000, null, { ref: false })]);
    if (service.exitCode === null) { service.kill(); await exited; }
  }
}

test().catch(error => { console.error(error); process.exitCode = 1; });
