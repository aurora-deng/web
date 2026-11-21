const assert = require('node:assert/strict');
const { readFileSync, existsSync } = require('node:fs');
const { mkdir, mkdtemp } = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { openBrowser } = require('./account_navigation_browser.cjs');

const root = path.resolve(__dirname, '../../..');
const site = path.join(root, 'frontend/phase11');
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.webp': 'image/webp' };

async function run() {
  let account = '7';
  let companion = false;
  let siteScope = 'a'.repeat(64);
  let privateHistory = [];
  const model = [{ accountId: '42', name: 'Fixture LLM', runtime: 'fixture' }];
  const inference = new Map();
  const remote = [];
  const native = new Map();
  const errors = [];
  const server = http.createServer(async (request, response) => {
    const { pathname } = new URL(request.url, 'http://localhost');
    const accept = (body, status = 200) => {
      response.writeHead(status, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify(body));
    };
    async function data() {
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      return JSON.parse(Buffer.concat(chunks));
    }
    try {
      if (pathname.startsWith('/phase11/')) {
        const name = path.resolve(site, pathname.slice('/phase11/'.length), pathname.endsWith('/') ? 'index.html' : '');
        if (!name.startsWith(site + path.sep) || !existsSync(name)) return accept({}, 404);
        response.writeHead(200, { 'Content-Type': mime[path.extname(name)] ?? 'application/octet-stream' });
        return response.end(readFileSync(name));
      }
      if (pathname === '/api/me') return accept({ user: { id: account, displayName: 'Fixture ' + account }, csrfToken: 'web-fixture' });
      if (pathname === '/api/local/session/info') return companion
        ? accept({ available: true, authenticated: true, accountId: account, sessionVersion: 1, csrfToken: 'local-fixture', siteScope }) : accept({}, 404);
      const local = '/api/local/assistant';
      if (pathname.startsWith(local)) {
        assert.equal(request.headers['x-local-csrf'], 'local-fixture');
        if (pathname !== local + '/capabilities') assert.equal(request.headers['x-nebula-account'], account);
      }
      if (pathname === local + '/capabilities') return accept({ available: true, accountId: account, preferenceInference: true, contextCompaction: true });
      if (pathname === '/api/ai/web/models' || pathname === local + '/models') return accept(model);
      if (pathname === local + '/settings') return accept({ aiUserId: '42' });
      if (pathname === local + '/threads') return accept([{ id: 'private-test', title: '私人测试' }]);
      if (pathname === local + '/threads/private-test/messages') return accept(privateHistory.concat([...native.values()].flatMap((item, index) => [
        { id: String(index * 2 + 101), requestId: item.requestId, role: 'user', body: item.text, state: 'completed' },
        { id: String(index * 2 + 102), requestId: item.requestId, role: 'assistant', body: '本机测试回答', state: 'completed' },
      ])));
      if (pathname === local + '/generations' && request.method === 'POST') {
        const value = await data();
        native.set(value.requestId, value);
        return accept({ id: value.requestId });
      }
      if (pathname.startsWith(local + '/generations/')) return accept({ state: 'completed', body: '本机测试回答', citations: [] });
      for (const [prefix, mode] of [['/api/ai/web/generations', 'web'], [local + '/preference-inference', 'private']]) {
        if (pathname === prefix && request.method === 'POST') {
          const body = await data();
          if (mode === 'web') {
            assert.equal(request.headers['x-csrf-token'], 'web-fixture');
            assert.equal(body.nonConfidentialConsent, 'true');
          }
          const classifier = body.content0.includes('偏好分类');
          const compressor = body.maxOutputTokens === '320';
          remote.push({ mode, account, classifier, compressor, body });
          inference.set(body.requestId, compressor ? '{"summary":"历史验证：继续采集 ACK。","memory":[1]}' :
            classifier ? '{}' : '网页测试回答');
          return accept({}, 202);
        }
        if (pathname.startsWith(prefix + '/')) {
          const id = pathname.slice(prefix.length + 1);
          if (request.method === 'DELETE') { inference.delete(id); return accept({ ok: true }); }
          return accept({ state: 'completed', pieces: [inference.get(id)], next: 1 });
        }
      }
      return accept([]);
    } catch (error) {
      errors.push(error.message);
      return accept({ message: error.message }, 500);
    }
  });

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = 'http://127.0.0.1:' + server.address().port;
  const builds = path.join(root, 'build-function3');
  await mkdir(builds, { recursive: true });
  let browser;
  try {
    browser = await openBrowser(await mkdtemp(path.join(builds, 'personal-agent-edge-')));
    const { send, evaluate, until } = browser;
    const page = '#private-friend';
    async function navigate(suffix) {
      await send('Page.navigate', { url: origin + '/phase11/assistant/' + suffix });
    }
    async function settings() {
      await until(`(() => { const button=document.querySelector('${page} [data-personalization]'); return button && !button.hidden && !button.disabled; })()`, 'personalization button');
      await evaluate(`document.querySelector('${page} [data-personalization]').click()`);
      await until('document.querySelector(".personal-agent-settings [name=enabled]:not(:disabled)")', 'durable preference dataset');
    }
    async function enable() {
      await evaluate('document.querySelector(".personal-agent-settings [name=enabled]").click()');
      await until('document.querySelector(".personal-agent-settings [role=status]").textContent.includes("已保存")', 'profile updated');
    }
    const close = () => evaluate('document.querySelector(".personal-agent-settings").close()');
    async function query(text, web = true) {
      const value = JSON.stringify(text);
      if (web) await evaluate(`document.querySelector('${page} [data-web-consent]').checked=true`);
      await evaluate(`(() => {
        const composer=document.querySelector('${page} .assistant-composer');
        composer.querySelector('textarea').value=${value};composer.requestSubmit();
      })()`);
      await until(web
        ? `document.querySelector('${page} [role=status]').textContent.includes('已保存')&&document.querySelector('${page} [data-stop]').disabled`
        : `document.querySelector('${page} [role=status]').textContent.includes('回答已保存在本机')`, 'assistant response completed');
    }

    await navigate('?mode=web');
    await settings();
    assert.equal(await evaluate('document.querySelector(".personal-agent-settings [name=enabled]").checked'), false);
    await enable();
    await evaluate(`(() => {
      const form=document.querySelector('.personal-agent-settings form');form.querySelector('input').value='PostgreSQL 事务等待是我正在排查的课题';form.requestSubmit();
    })()`);
    await until('document.querySelector(".personal-agent-settings ul:last-of-type").textContent.includes("PostgreSQL")', 'note stored');
    await close();
    await query('以后我喜欢简短回答，优先分点');
    assert.deepEqual(remote.map(result => result.classifier), [true, false]);
    assert.match(remote[1].body.content0, /回答简短.*分点呈现/);
    assert.equal(remote[1].body.maxOutputTokens, '280');

    await navigate('?mode=web&visit=2');
    await settings();
    await until('document.querySelector(".personal-agent-settings ul:first-of-type").textContent.includes("分点呈现")', 'memory after reload');
    await close();
    await query('PostgreSQL 事务等待怎么检查');
    assert(!remote.at(-2).body.content1.includes('课题'));
    assert.match(remote.at(-1).body.content0, /PostgreSQL 事务等待是我正在排查的课题/);

    const archived = Array.from({ length: 18 }, (_, index) => ({
      sequence: String(index + 1), role: index % 2 ? 'assistant' : 'user',
      content: index === 0 ? '以后我喜欢用 JavaScript 代码示例。' : '历史原文-' + index + '。' + '项目数据'.repeat(140),
    }));
    await evaluate(`(() => {
      const key='nebula:xiaoyan:web:7';
      const saved=JSON.parse(sessionStorage.getItem(key));
      saved.threads=[{id:'web-long',title:'长会话',messages:${JSON.stringify(archived)}}];
      saved.selected='web-long';sessionStorage.setItem(key,JSON.stringify(saved));
    })()`);
    await navigate('?mode=web&visit=long');
    await settings();
    await close();
    const beforeLong = remote.length;
    await query('继续刚才的 ACK 排查');
    assert.equal(remote.length, beforeLong + 2);
    assert.equal(remote.at(-2).compressor, true);
    assert.match(remote.at(-1).body.content0, /历史验证：继续采集 ACK/);
    assert(!Object.values(remote.at(-1).body).some(value => String(value).startsWith('历史原文-1。')));
    const saved = await evaluate('JSON.parse(sessionStorage.getItem("nebula:xiaoyan:web:7")).threads[0].checkpoint');
    assert(Number(saved.through) > 0 && saved.summary.includes('ACK'));
    await settings();
    assert.match(await evaluate('document.querySelector(".personal-agent-settings ul").textContent'), /JavaScript.*来自会话压缩/);
    await close();

    account = '8';
    await navigate('?mode=web&visit=3');
    await settings();
    assert.equal(await evaluate('document.querySelector(".personal-agent-settings [name=enabled]").checked'), false);
    await close();
    const before = remote.length;
    await query('PostgreSQL 事务等待怎么检查');
    assert.equal(remote.length, before + 1);
    assert.equal(remote.at(-1).body.role0, 'user');

    account = '7';
    companion = true;
    await navigate('?visit=4');
    await settings();
    assert.equal(await evaluate('document.querySelector(".personal-agent-settings [name=enabled]").checked'), false);
    await enable();
    await close();
    await query('以后我偏好英文回答', false);
    assert.equal(remote.at(-1).mode, 'private');
    assert.equal(remote.at(-1).classifier, true);
    assert.equal(native.size, 1);
    assert.match([...native.values()][0].personalization, /默认用英文/);
    assert(![...native.values()][0].personalization.includes('PostgreSQL'));
    privateHistory = archived.map(row => ({ id: row.sequence, role: row.role, body: row.content, state: 'completed' }));
    await query('继续私人会话的长上下文', false);
    assert(remote.at(-1).compressor && remote.at(-1).mode === 'private');
    assert.match([...native.values()].at(-1).historySummary, /历史验证/);
    assert(Number([...native.values()].at(-1).historyThrough) > 0);
    siteScope = 'b'.repeat(64);
    await navigate('?visit=5');
    await settings();
    assert.equal(await evaluate('document.querySelector(".personal-agent-settings [name=enabled]").checked'), false);
    await close();
    assert.deepEqual(errors, []);
    console.log('PASS Edge 两端长会话压缩、历史偏好来源、真实 IndexedDB 恢复、账号及上游隔离（模型替身）');
  } finally {
    browser?.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
}

run().catch(error => { console.error(error); process.exitCode=1; });
