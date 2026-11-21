const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { mkdirSync, mkdtempSync, writeFileSync } = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { createInterface } = require('node:readline');
const { setTimeout: wait } = require('node:timers/promises');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

const root = path.resolve(__dirname, '../..');
const companionBinary = process.env.YANTU_COMPANION_EXE;

async function verifyCompanion(browser, data, secureOrigin, title, scopeUrl) {
  const reservation = net.createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const origin = 'http://localhost:' + port;
  const stopFile = path.join(data, 'stop-companion');
  const client = spawn(companionBinary, ['--server', 'localhost', '--connect-host', '127.0.0.1',
    '--tls-port', new URL(secureOrigin).port, '--port', String(port),
    '--certificate-sha256', process.env.YANTU_COMPANION_PIN,
    '--frontend', path.join(root, 'frontend/phase11'), '--data-dir', path.join(data, 'client-data'),
    '--stop-file', stopFile], { cwd: root, stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true });
  const exited = new Promise(resolve => client.once('exit', resolve));
  const output = createInterface({ input: client.stdout });
  const context = await browser.newContext();
  try {
    const startup = await Promise.race([
      new Promise(resolve => output.on('line', line => { if (line.startsWith(origin + '/phase11/#companion=')) resolve(line); })),
      exited.then(() => { throw new Error('Native companion exited'); }),
      wait(10000, undefined, { ref: false }).then(() => { throw new Error('Native companion startup timed out'); }),
    ]);
    const page = await context.newPage();
    await page.goto(origin + '/phase11/');
    const token = new URLSearchParams(new URL(startup).hash.slice(1)).get('companion');
    const loginResult = await page.evaluate(async launchToken => {
      const post = (url, body) => fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body) });
      const bootstrap = await post('/api/local/session/bootstrap', { token: launchToken });
      if (!bootstrap.ok) return bootstrap.status;
      return (await post('/api/auth/login', { username: 'synthetic', password: 'fixture-only' })).status;
    }, token);
    assert.equal(loginResult, 200);
    await page.goto(origin + '/phase11/study/');
    const iframe = page.locator('#study-surface iframe');
    await iframe.waitFor();
    assert.equal(await iframe.getAttribute('src'), scopeUrl);
    const frame = await iframe.elementHandle().then(handle => handle.contentFrame());
    await frame.locator('body.app-ready').waitFor();
    assert(await frame.evaluate(title => state.notes.some(note => note.title === title), title));
    const attachment = await frame.evaluate(async () => {
      const info = await PrepTransport.fetch('/api/files/browser-companion-shared', { method: 'HEAD' });
      const result = await PrepTransport.fetch('/api/files/browser-companion-shared', { headers: { Range: 'bytes=0-3' } });
      return { length: info.headers.get('Content-Length'), status: result.status, text: await result.text() };
    });
    assert.deepEqual(attachment, { length: '14', status: 206, text: 'test' });
    const switchAccount = async username => {
      await page.getByRole('link', { name: '切换账号', exact: true }).click();
      await page.getByRole('heading', { name: '切换 NEBULA 账号', exact: true }).waitFor();
      await page.locator('#login-form [name=username]').fill(username);
      await page.locator('#login-form [name=password]').fill('fixture-only');
      await page.locator('#login-form button[type=submit]').click();
      await page.waitForURL(origin+'/phase11/study/');
      await page.frameLocator('#study-surface iframe').locator('body.app-ready').waitFor();
      const current = await iframe.elementHandle().then(handle => handle.contentFrame());
      return { url: await iframe.getAttribute('src'), hasNote: await current.evaluate(value => state.notes.some(note => note.title === value), title) };
    };
    const secondAccount = await switchAccount('fixture-second');
    assert.notEqual(secondAccount.url, scopeUrl);
    assert.equal(secondAccount.hasNote, false);
    assert.deepEqual(await switchAccount('synthetic'), { url: scopeUrl, hasNote:true });
    console.log('PASS native companion TLS, same account notes, attachment HEAD and Range, and A → B → A account switch');
  } finally {
    await context.close();
    writeFileSync(stopFile, '', 'utf8');
    await Promise.race([exited, wait(5000, undefined, { ref: false })]);
    if (client.exitCode === null) { client.kill(); await exited; }
    output.close();
  }
}

async function main() {
  const output = path.join(root, 'build-function3');
  mkdirSync(output, { recursive: true });
  const data = mkdtempSync(path.join(output, 'yantu-account-browser-'));
  const tls = companionBinary ? ['--tls-cert', process.env.YANTU_COMPANION_CERT,
    '--tls-key', process.env.YANTU_COMPANION_KEY] : [];
  const child = spawn('python', ['-B', 'tests/client/yantu_account_workspace_test.py', '--serve',
    '--work-dir', data, '--port', '0', ...tls], { cwd: root, stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true });
  const exited = new Promise(resolve => child.once('exit', resolve));
  const lines = createInterface({ input: child.stdout });
  let browser;
  try {
    const descriptor = await Promise.race([
      new Promise(resolve => lines.once('line', resolve)),
      exited.then(() => { throw new Error('Study HTTP lab exited'); }),
      wait(10000, undefined, { ref: false }).then(() => { throw new Error('Study HTTP lab start timed out'); }),
    ]);
    const { url: base, tls: secureOrigin } = JSON.parse(descriptor);
    browser = await chromium.launch({ headless: true, executablePath: process.env.CHAT_TEST_BROWSER });
    const deviceOne = await browser.newContext();
    const deviceTwo = await browser.newContext();
    const accountOne = await deviceOne.newPage();
    const accountTwo = await deviceTwo.newPage();
    const login = (context, token) => context.addCookies([{ name: 'phase11_session', value: token, url: base, httpOnly: true }]);
    const app = page => page.frameLocator('#study-surface iframe');
    const note = '仅合成数据 · 两设备的学习笔记';
    const errors = [];
    const networkErrors = [];
    for (const page of [accountOne, accountTwo]) {
      page.on('pageerror', error => errors.push(error.message));
      page.on('requestfailed', request => networkErrors.push([new URL(request.url()).pathname, request.failure()?.errorText]));
      page.on('response', response => {
        if (response.status() >= 400 && response.request().resourceType() === 'script') {
          networkErrors.push([new URL(response.url()).pathname, response.status()]);
        }
      });
    }

    await deviceOne.addInitScript(() => {
      // Seed the old, unscoped keys. A hosted account must never import them.
      if (!localStorage.getItem('kaoyan-prep-state')) {
        localStorage.setItem('kaoyan-prep-state', JSON.stringify({ notes: [{ id: 'legacy', title: 'legacy offline note' }] }));
      }
    });
    await accountOne.goto(base + '/phase11/study/');
    await accountOne.getByRole('heading', { name: '登录后打开研途' }).waitFor();
    await login(deviceOne, 'synthetic_account7_device1');
    await login(deviceTwo, 'synthetic_account7_device2');
    await Promise.all([accountOne.reload(), accountTwo.goto(base + '/phase11/study/')]);
    for (const page of [accountOne, accountTwo]) {
      try {
        await app(page).locator('.nav-item[data-view="resources"]').waitFor({ state: 'attached', timeout: 15000 });
      } catch (error) {
        const diagnostic = await Promise.all(page.frames().map(frame => frame.evaluate(() => ({
          url: location.href, text: document.body?.innerText.slice(0, 250),
        })).catch(() => ({ url: frame.url() }))));
        throw new Error(`Study workspace did not mount: ${JSON.stringify(diagnostic)}, errors ${JSON.stringify(errors)}, network ${JSON.stringify(networkErrors.slice(-10))}`, {cause: error});
      }
      assert.equal(await page.locator('a[href*="18771"]').count(), 0);
    }
    const frameOne = await accountOne.locator('#study-surface iframe').elementHandle().then(handle => handle.contentFrame());
    const frameTwo = await accountTwo.locator('#study-surface iframe').elementHandle().then(handle => handle.contentFrame());
    await frameOne.locator('body.app-ready').waitFor();
    await frameTwo.locator('body.app-ready').waitFor();
    assert.deepEqual(await frameOne.evaluate(() => state.notes), []);
    await frameOne.evaluate(async title => {
      await PrepLocal.jsonRequest('/api/time');
      state.notes.push({ id: 'browser-shared-note', title, content: 'Synthetic', createdAt: new Date().toISOString() });
      saveState();
      await PrepLocal.flush();
    }, note);
    const saved = await frameOne.evaluate(() => PrepLocal.jsonRequest('/api/state'));
    assert(saved.state.notes.some(record => record.title === note));
    await frameTwo.waitForFunction(title => state.notes.some(record => record.title === title), note, { timeout: 20000 });
    if (companionBinary) {
      const attachmentStatus = await frameOne.evaluate(async () => (await PrepTransport.fetch('/api/files/browser-companion-shared', {
        method: 'PUT', headers: { 'Content-Type': 'text/plain' }, body: 'test companion',
      })).status);
      assert.equal(attachmentStatus, 200);
      await verifyCompanion(browser, data, secureOrigin, note, await accountOne.locator('#study-surface iframe').getAttribute('src'));
    }
    await accountTwo.getByRole('button', { name: '下载位置', exact: true }).click();
    await app(accountTwo).locator('#download-settings[open]').waitFor();
    await app(accountTwo).locator('#download-settings button[type=submit]').click();
    const pdf = await frameTwo.evaluate(async () => (await import(YantuEnvironment.pathname('/vendor/pdfjs-5.6.205/build/pdf.min.mjs'))).version);
    assert.equal(pdf, '5.6.205');
    const original = await accountTwo.locator('#study-surface iframe').getAttribute('src');
    await login(deviceTwo, 'synthetic_account8_device1');
    await frameTwo.evaluate(() => PrepTransport.fetch('/api/state')).catch(error => {
      if (!error.message.includes('detached')) throw error;
    });
    await accountTwo.waitForFunction(url => {
      const src = document.querySelector('#study-surface iframe')?.getAttribute('src');
      return src && src !== url;
    }, original);
    const newFrame = await accountTwo.locator('#study-surface iframe').elementHandle().then(handle => handle.contentFrame());
    await newFrame.waitForFunction(() => window.PrepLocal?.jsonRequest && typeof state !== 'undefined');
    assert.deepEqual(await newFrame.evaluate(() => state.notes), []);
    assert.deepEqual(errors, []);
    console.log('PASS embedded study, cross-browser synchronization, download settings, PDF assets and account switch');
  } finally {
    await browser?.close();
    child.stdin.end();
    await Promise.race([exited, wait(5000, undefined, { ref: false })]);
    if (child.exitCode === null) { child.kill(); await exited; }
    lines.close();
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
