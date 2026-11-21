const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { existsSync, readFileSync } = require('node:fs');
const { mkdir, mkdtemp } = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { setTimeout: wait } = require('node:timers/promises');

const root = path.resolve(__dirname, '../../..');
const site = path.join(root, 'frontend/phase11');
const browserPath = process.env.CHAT_TEST_BROWSER ||
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.css': 'text/css', '.png': 'image/png', '.webp': 'image/webp' };
const recordLoginFrames = `<script>
  window.visibleLoginFrames = 0;
  function record() {
    if (document.getElementById('auth-panel')?.getClientRects().length) window.visibleLoginFrames++;
    requestAnimationFrame(record);
  }
  requestAnimationFrame(record);
</script>`;

async function until(read, description) {
  for (let attempts = 0; attempts < 150; attempts++) {
    const value = await read();
    if (value) return value;
    await wait(80);
  }
  throw new Error('Timed out waiting for ' + description);
}

async function openBrowser(profile) {
  const browser = spawn(browserPath, [
    '--headless=new', '--disable-gpu', '--disable-background-networking', '--no-first-run',
    '--remote-debugging-port=0', '--user-data-dir=' + profile,
    '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1, EXCLUDE localhost', 'about:blank',
  ], { windowsHide: true, stdio: 'ignore' });
  const file = path.join(profile, 'DevToolsActivePort');
  let wire;
  try {
    const port = await until(() => existsSync(file) && readFileSync(file, 'utf8').split(/\r?\n/)[0], 'DevTools port');
    const targets = await (await fetch('http://127.0.0.1:' + port + '/json/list')).json();
    wire = new WebSocket(targets.find(target => target.type === 'page').webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      wire.addEventListener('open', resolve, { once: true });
      wire.addEventListener('error', reject, { once: true });
    });
  } catch (error) {
    browser.kill();
    throw error;
  }
  let serial = 0;
  const pending = new Map();
  wire.addEventListener('message', event => {
    const response = JSON.parse(event.data);
    pending.get(response.id)?.(response);
    pending.delete(response.id);
  });
  const send = (method, params = {}) => new Promise(resolve => {
    const id = ++serial;
    pending.set(id, resolve);
    wire.send(JSON.stringify({ id, method, params }));
  }).then(response => {
    if (response.error) throw new Error(response.error.message);
    return response.result;
  });
  const evaluate = async expression => {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
    return result.result.value;
  };
  return {
    send, evaluate,
    until: (expression, description) => until(() => evaluate(expression).catch(() => false), description),
    close() { wire.close(); browser.kill(); },
  };
}

async function run() {
  let sessionStatus = 200;
  const scope = 'a'.repeat(32);
  const studyUrl = '/api/study/' + scope + '/';
  const server = http.createServer(async (request, response) => {
    const route = new URL(request.url, 'http://localhost').pathname;
    const json = (status, data) => {
      response.writeHead(status, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify(data));
    };
    if (route.startsWith('/phase11/')) {
      const name = path.resolve(site, route.slice('/phase11/'.length), route.endsWith('/') ? 'index.html' : '');
      if (!name.startsWith(site + path.sep) || !existsSync(name)) return json(404, {});
      response.writeHead(200, { 'Content-Type': types[path.extname(name)] ?? 'application/octet-stream' });
      if (route === '/phase11/') return response.end(readFileSync(name, 'utf8').replace('<head>', '<head>' + recordLoginFrames));
      return response.end(readFileSync(name));
    }
    if (route === '/api/me') {
      const status = sessionStatus;
      await wait(600);
      return json(status, status === 200 ? { user: { id: '7', displayName: 'Fixture' }, csrfToken: 'synthetic' } : {});
    }
    if (route === '/api/tree/posts') return json(200, { posts: [], total: 0, next: null });
    if (route === '/api/tree/ranking') return json(200, { entries: [] });
    if (route === '/api/me/notification-settings') return json(200, { soundEnabled: false });
    if (route === '/api/study/session') return json(200, { accountId: '7', scope, url: studyUrl });
    if (route === studyUrl) {
      response.writeHead(200, { 'Content-Type': 'text/html' });
      return response.end('<title>Study fixture</title>');
    }
    if (route === '/api/local/session/info') return json(404, {});
    return json(200, []);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = 'http://127.0.0.1:' + server.address().port;
  const builds = path.join(root, 'build-function3');
  await mkdir(builds, { recursive: true });
  let browser;
  try {
    browser = await openBrowser(await mkdtemp(path.join(builds, 'account-navigation-edge-')));
    const { send, evaluate, until: check } = browser;
    await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: origin + '/phase11/tree/#feed' });
    const chat = `document.querySelector('#nebula-navigation nav a[href="/phase11/"]')`;
    await check(`${chat}?.textContent.includes('缘续-聊天') && document.body.dataset.treeView === 'feed'`, 'chat link');
    assert(await evaluate(`document.querySelector('#nebula-navigation nav a[href="/phase11/tree/"]').textContent.includes('缘生-广场')`));
    const turn = `(() => {
      scrollTo(0, document.documentElement.scrollHeight);
      for (let index = 0; index < 2; index++) {
        document.body.dispatchEvent(new WheelEvent('wheel', { deltaY: 110, bubbles: true, cancelable: true }));
      }
      return document.querySelector('.community-main').getAnimations().some(animation =>
        animation.effect.getKeyframes().some(frame => frame.filter === 'blur(6px)'));
    })()`;
    assert.equal(await evaluate(turn), true);
    await check(`document.body.dataset.treeView === 'rank' && !document.querySelector('.community-main').hasAttribute('aria-busy')`, 'wheel transition');
    await evaluate(`${chat}.click()`);
    const isChatReady = `location.pathname === '/phase11/' && document.getElementById('chat-panel')?.hidden === false`;
    await check(isChatReady, 'authorized chat');
    assert.equal(await evaluate('window.visibleLoginFrames'), 0);
    await evaluate(`document.querySelector('#nebula-navigation nav a[href="/phase11/study/"]').click()`);
    await check(`document.querySelector('#study-surface iframe')?.getAttribute('src') === '${studyUrl}'`, 'identity-bound study');
    await evaluate(`${chat}.click()`);
    await check(isChatReady, 'authorized chat from study');
    assert.equal(await evaluate('window.visibleLoginFrames'), 0);
    sessionStatus = 503;
    await send('Page.reload', {});
    await check(`document.getElementById('account-check-retry')?.getClientRects().length`, 'retry state');
    assert.equal(await evaluate('window.visibleLoginFrames'), 0);
    sessionStatus = 401;
    await evaluate(`document.getElementById('account-check-retry').click()`);
    await check(`document.getElementById('auth-panel')?.getClientRects().length`, 'anonymous login');
    sessionStatus = 200;
    await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
    await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: 'reduce' }] });
    await send('Page.navigate', { url: origin + '/phase11/tree/#feed' });
    await check(`document.querySelector('.community-main') && document.body.dataset.treeView === 'feed'`, 'mobile feed');
    await wait(800);
    assert.equal(await evaluate(turn), false);
    await check(`document.body.dataset.treeView === 'rank'`, 'reduced-motion wheel transition');
    assert(await evaluate('document.documentElement.scrollWidth <= 390'));
    console.log('PASS Edge account navigation, deferred login, retry, blur and mobile reduced motion');
  } finally {
    browser?.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
}

if (require.main === module) run().catch(error => { console.error(error); process.exitCode = 1; });
module.exports = { openBrowser, until };
