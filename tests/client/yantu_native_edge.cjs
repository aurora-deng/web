const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { mkdir, mkdtemp, writeFile } = require('node:fs/promises');
const net = require('node:net');
const path = require('node:path');
const readline = require('node:readline');
const { setTimeout: wait } = require('node:timers/promises');
const { openBrowser } = require('../function3/frontend/account_navigation_browser.cjs');

const root = path.resolve(__dirname, '../..');
const directory = path.join(root, 'build-function3');
const noteTitle = 'Function3.3 synthetic A-only note';

function start(exe, args, parse, pipes) {
  const child = spawn(exe, args, { cwd: root, stdio: pipes, windowsHide: true });
  const lines = readline.createInterface({ input: child.stdout });
  const exited = new Promise(resolve => child.once('exit', resolve));
  const ready = Promise.race([
    new Promise((resolve, reject) => {
      child.once('error', reject);
      lines.on('line', line => {
        try {
          const parsed = parse(line);
          if (parsed) resolve(parsed);
        } catch (error) { reject(error); }
      });
    }),
    exited.then(code => { throw new Error('Test child exited early with code ' + code); }),
    wait(12000, undefined, { ref: false }).then(() => { throw new Error('Test child startup timeout'); }),
  ]);
  return { child, ready, async close() {
    await Promise.race([exited, wait(4500, undefined, { ref: false })]);
    if (child.exitCode === null && child.kill()) await exited;
    lines.close();
  } };
}

async function main() {
  await mkdir(directory, { recursive: true });
  const work = await mkdtemp(path.join(directory, 'function33-native-edge-'));
  const gateway = start('python', ['-B', 'tests/client/yantu_account_workspace_test.py', '--serve',
    '--work-dir', work, '--port', '0', '--tls-cert', process.env.YANTU_COMPANION_CERT, '--tls-key', process.env.YANTU_COMPANION_KEY],
    line => JSON.parse(line), ['pipe', 'pipe', 'inherit']);
  let browser, native, stopFile;
  try {
    const test = await gateway.ready;
    const portServer = net.createServer();
    await new Promise(resolve => portServer.listen(0, '127.0.0.1', resolve));
    const port = portServer.address().port;
    await new Promise(resolve => portServer.close(resolve));
    const nativeOrigin = 'http://localhost:' + port;
    stopFile = path.join(work, 'stop');
    native = start(process.env.YANTU_COMPANION_EXE, ['--server', 'localhost', '--connect-host', '127.0.0.1',
      '--tls-port', new URL(test.tls).port, '--port', String(port), '--certificate-sha256', process.env.YANTU_COMPANION_PIN,
      '--frontend', path.join(root, 'frontend/phase11'), '--data-dir', path.join(work, 'native-data'),
      '--stop-file', stopFile], line => line.startsWith(nativeOrigin + '/phase11/#companion=') && line,
      ['ignore', 'pipe', 'inherit']);
    browser = await openBrowser(path.join(work, 'edge-profile'));
    const { send, evaluate, until } = browser;
    const study = '/phase11/study/';
    const form = `document.getElementById('login-form')`;
    const fill = (username, password = 'fixture-only') => evaluate(`(() => {
      const form = ${form};
      form.elements.username.value = ${JSON.stringify(username)};
      form.elements.password.value = ${JSON.stringify(password)};
      form.requestSubmit();
    })()`);
    const activeFrame = `document.querySelector('#study-surface iframe')`;
    const readyFrame = `location.pathname === '${study}' && ${activeFrame}?.contentDocument?.body?.classList.contains('app-ready')`;
    const showStudy = async () => {
      await send('Page.navigate', { url: nativeOrigin + study });
      await until(readyFrame, 'study app');
    };
    const currentScope = () => evaluate(`${activeFrame}.getAttribute('src')`);
    const inner = expression => evaluate(`${activeFrame}.contentWindow.eval(${JSON.stringify(expression)})`);
    const notePresent = () => inner(`state.notes.some(note => note.title === ${JSON.stringify(noteTitle)})`);
    const switchPage = async () => {
      await evaluate(`document.getElementById('study-switch-account').click()`);
      await until(`document.getElementById('login-title')?.textContent.includes('切换 NEBULA 账号')`, 'switch entry');
    };
    const assertAccount = async id => assert.equal(await evaluate(`fetch('/api/study/session').then(r=>r.json()).then(x=>x.accountId)`), id);

    await send('Page.navigate', { url: await native.ready });
    await until(`${form}?.getClientRects().length`, 'initial login form');
    await fill('synthetic');
    await until(`document.getElementById('chat-panel')?.getClientRects().length`, 'native logged-in chat');
    await showStudy();
    await assertAccount('7');
    const a = await currentScope();
    assert.equal(await notePresent(), false);
    await inner(`(async () => {
      state.notes.push({ id: 'function33-note', title: ${JSON.stringify(noteTitle)}, content: 'Synthetic' });
      saveState();
      await PrepLocal.flush();
    })()`);
    const upload = await inner(`PrepTransport.fetch('/api/files/native-sample',{method:'PUT',headers:{'Content-Type':'text/plain'},body:'synthetic file'}).then(r=>r.status)`);
    assert.equal(upload, 200);
    const fileHeaders = await inner(`PrepTransport.fetch('/api/files/native-sample',{method:'HEAD'}).then(r=>[r.status,r.headers.get('Content-Length')])`);
    assert.deepEqual(fileHeaders, [200, '14']);
    const range = await inner(`PrepTransport.fetch('/api/files/native-sample',{headers:{Range:'bytes=0-3'}}).then(async r=>[r.status,await r.text()])`);
    assert.deepEqual(range, [206, 'synt']);

    await switchPage();
    await fill('fixture-second', 'incorrect-fixture');
    await until(`document.getElementById('auth-status').textContent.length > 0 && !document.querySelector('#login-form button').disabled`, 'login rejection');
    assert.equal(await evaluate(`fetch('/api/me').then(r=>r.json()).then(x=>x.user.id)`), '7');
    await fill('fixture-second');
    await until(readyFrame, 'account B');
    await assertAccount('8');
    assert.notEqual(await currentScope(), a);
    assert.equal(await notePresent(), false);
    assert.equal(await evaluate(`fetch(${JSON.stringify(a + 'api/state')}).then(r=>r.status)`), 403);

    await switchPage();
    await fill('synthetic');
    await until(readyFrame, 'restored account A');
    await assertAccount('7');
    assert.equal(await currentScope(), a);
    assert.equal(await notePresent(), true);
    console.log('PASS native Windows TLS, A → B → A, failed login preservation, isolated notes and attachments');
  } finally {
    browser?.close();
    if (native) { await writeFile(stopFile, ''); await native.close(); }
    gateway.child.stdin.end();
    await gateway.close();
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
