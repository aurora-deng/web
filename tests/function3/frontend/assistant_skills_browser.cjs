// Real Edge and NEBULA pages with synthetic account, model and write endpoints.
// Use CHAT_TEST_BROWSER and PLAYWRIGHT_MODULE to point to existing tools.
const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
const { readFile } = require('node:fs/promises');
const path = require('node:path');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

const site = path.resolve(__dirname, '../../../frontend/phase11');
const origin = 'http://127.0.0.1:18782';
const me = { id: '7', username: 'fixture', displayName: '测试账号' };
const scope = 'a'.repeat(32);
const study = '/api/study/' + scope + '/';
const local = '/api/local/assistant';
const localInference = local + '/preference-inference';
const webInference = '/api/ai/web/generations';
const chats = [['11', '考研同学'], ['12', '项目小组']].map(([id, title]) => ({
  id, title, displayTitle: title, kind: 'group', nextSequence: 1, unreadCount: 0,
}));
const categories = [{ id: '4', name: '考研', active: true }];
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.webp': 'image/webp', '.png': 'image/png' };

async function run() {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHAT_TEST_BROWSER });
  const requests = [];
  const replies = new Map();
  const messages = [];
  const chatEvents = new EventEmitter();
  const posts = [];
  const errors = [];
  let studyCalls = 0;
  let currentUser = me;
  let companion = false;
  let page;
  try {
    page = await browser.newPage({ viewport: { width: 1280, height: 900 }, reducedMotion: 'reduce' });
    page.setDefaultTimeout(12000);
    page.on('pageerror', error => errors.push(error.message));
    await page.routeWebSocket('**/ws', socket => socket.onMessage(raw => {
      const payload = JSON.parse(raw);
      if (payload.type === 'chat.send') {
        messages.push(payload);
        chatEvents.emit('send');
      }
    }));
    await page.route('**/*', async route => {
      const request = route.request();
      const url = new URL(request.url());
      const { pathname } = url;
      const json = (value, status = 200) => route.fulfill({ status, json: value });
      if (url.origin !== origin) return route.abort();
      if (pathname.startsWith('/phase11/')) {
        const file = path.resolve(site, pathname.slice('/phase11/'.length), pathname.endsWith('/') ? 'index.html' : '');
        if (!file.startsWith(site + path.sep)) return json({}, 404);
        const body = await readFile(file).catch(() => undefined);
        if (!body) return json({}, 404);
        return route.fulfill({ body, contentType: mime[path.extname(file)] || 'application/octet-stream' });
      }
      if (pathname === '/api/me') return json({ user: currentUser, csrfToken: 'test-only-csrf' });
      if (pathname === '/api/local/session/info') return companion
        ? json({ available: true, authenticated: true, accountId: currentUser.id, sessionVersion: 1,
          csrfToken: 'test-local-csrf', siteScope: 'a'.repeat(64) })
        : json({}, 404);
      if (pathname === local + '/capabilities') return json({ available: true, accountId: currentUser.id, preferenceInference: true });
      if (pathname === local + '/settings') return json({ aiUserId: '42' });
      if (pathname === '/api/ai/web/models' || pathname === local + '/models') return json([{ accountId: '42', name: '模型替身' }]);
      if ([webInference, localInference].includes(pathname) && request.method() === 'POST') {
        const payload = request.postDataJSON();
        requests.push({ pathname, payload, headers: request.headers() });
        replies.set(payload.requestId, '可编辑的测试草稿');
        return json({}, 202);
      }
      if (pathname.startsWith(webInference + '/') || pathname.startsWith(localInference + '/')) {
        if (request.method() === 'DELETE') return json({});
        return json({ state: 'completed', pieces: [replies.get(pathname.split('/').at(-1))], next: 1 });
      }
      if (pathname === '/api/study/session') return json({ accountId: me.id, scope, url: study });
      if (pathname === study + 'api/assistant-summary') {
        studyCalls++;
        return json({ accountId: me.id, day: url.searchParams.get('day'), todayPlanned: 2,
          todayCompleted: 1, overdue: 3, nextSevenDays: 4, note: '不得流入模型的测试字段' });
      }
      if (pathname === '/api/conversations') return json(chats);
      if (pathname === '/api/tree/categories') return json(categories);
      if (pathname === '/api/tree/posts') return json({ posts: [], total: '0', next: '' });
      if (pathname === '/api/tree/ranking') return json({ entries: [] });
      if (pathname === '/api/activities' && request.method() === 'POST') {
        const fields = request.postDataJSON();
        posts.push({ csrf: request.headers()['x-csrf-token'], fields });
        return json({ id: '100', ...fields }, 201);
      }
      return json([]);
    });

    const field = name => page.locator('[data-' + name + ']');
    async function openAssistant(mode = 'web') {
      await page.goto(origin + '/phase11/assistant/' + (mode === 'web' ? '?mode=web' : ''));
      await page.locator('.assistant-skills summary').click();
      await page.waitForSelector('[data-run-skill]:enabled');
    }
    async function generate(skill, brief, studyStats = false) {
      await field('skill').selectOption(skill);
      await field('brief').fill(brief);
      if (studyStats) await field('include-study').check();
      await field('run-skill').click();
      await field('skill-draft').waitFor();
      assert.equal(await field('skill-draft').inputValue(), '可编辑的测试草稿');
      const { pathname, payload, headers } = requests.at(-1);
      assert.equal(payload.messageCount, '2');
      assert.equal(pathname, companion ? localInference : webInference);
      if (companion) {
        assert.equal(headers['x-local-csrf'], 'test-local-csrf');
        assert.equal(headers['x-nebula-account'], currentUser.id);
        assert.equal(payload.nonConfidentialConsent, undefined);
      } else {
        assert.equal(headers['x-csrf-token'], 'test-only-csrf');
        assert.equal(payload.nonConfidentialConsent, 'true');
      }
      return payload;
    }
    async function transfer(target, draft, destinationPage) {
      await field('skill-draft').fill(draft);
      await field('read-targets').click();
      await field('skill-target').selectOption(target);
      await Promise.all([page.waitForURL(url => url.pathname === destinationPage), field('handoff').click()]);
    }

    await openAssistant();
    await field('brief').fill('这段输入尚未同意发送');
    await field('run-skill').click();
    await page.waitForFunction(() => document.querySelector('[data-skill-status]').textContent.includes('发送范围'));
    assert.equal(requests.length, 0);
    await field('web-consent').check();
    const plan = await generate('kaoyan-week-plan', '这周准备计网', true);
    assert.match(plan.content0, /七天计划/);
    assert.match(plan.content1, /"todayCompleted":1/);
    assert.doesNotMatch(plan.content1, /不得流入模型/);
    assert.equal(studyCalls, 1);
    const review = await generate('kaoyan-review', '混淆了接收窗口和拥塞窗口');
    assert.match(review.content0, /错因/);
    assert.equal(review.content1, '混淆了接收窗口和拥塞窗口');
    assert.equal(studyCalls, 1);

    await generate('chat-draft', '和同学约时间复盘');
    await transfer('11', '周末一起复盘计网错题？', '/phase11/');
    const input = page.locator('#message-form [name=message]');
    await input.waitFor();
    await page.waitForFunction(() => document.querySelector('#message-form [name=message]').value.includes('周末'));
    assert.equal(await page.locator('#composer-destination').innerText(), chats[0].title);
    assert.equal(new URL(page.url()).searchParams.has('xiaoyanDraft'), false);
    assert.equal(messages.length, 0);
    await page.locator('#conversation-list button').filter({ hasText: chats[1].title }).click();
    await page.locator('#message-form .send-button').click();
    assert.match(await page.locator('#toast-region').innerText(), /已绑定原会话/);
    assert.equal(messages.length, 0);
    await page.locator('#conversation-list button').filter({ hasText: chats[0].title }).click();
    await Promise.all([
      once(chatEvents, 'send', { signal: AbortSignal.timeout(12000) }),
      page.locator('#message-form .send-button').click(),
    ]);
    assert.equal(await input.inputValue(), '');
    assert.deepEqual(messages.map(({ conversationId, content }) => ({ conversationId, content })),
      [{ conversationId: 11, content: '周末一起复盘计网错题？' }]);

    await openAssistant();
    await field('web-consent').check();
    await generate('plaza-draft', '写下我今天的备考心得');
    await transfer('4', '今天把错题写成了自测卡。', '/phase11/tree/');
    await page.locator('#publish-dialog[open]').waitFor();
    assert.equal(await page.locator('#publish-form [name=body]').inputValue(), '今天把错题写成了自测卡。');
    assert.equal(await page.locator('#publish-category').inputValue(), '4');
    const confirmation = page.locator('#publish-form [name=publicConfirmed]');
    assert.equal(await confirmation.isChecked(), false);
    await page.locator('#publish-submit').click();
    assert.equal(posts.length, 0);
    await confirmation.check();
    await Promise.all([
      page.waitForResponse(response => response.url().endsWith('/api/activities') && response.status() === 201),
      page.locator('#publish-submit').click(),
    ]);
    assert.equal(posts.length, 1);
    assert.equal(posts[0].csrf, 'test-only-csrf');
    const { body, destination, categoryId, publicConfirmed, requestId } = posts[0].fields;
    assert.deepEqual({ body, destination, categoryId, publicConfirmed },
      { body: '今天把错题写成了自测卡。', destination: 'tree', categoryId: '4', publicConfirmed: 'true' });
    assert.match(requestId, /^[a-f0-9]{32}$/);

    await openAssistant();
    await field('web-consent').check();
    currentUser = { ...me, id: '8' };
    await field('brief').fill('账号切换后不能继续旧操作');
    await field('run-skill').click();
    await page.waitForFunction(() => document.querySelector('[data-skill-status]').textContent.includes('账号已变化'));
    assert.equal(requests.length, 4);

    currentUser = me;
    companion = true;
    await openAssistant('private');
    const privateDraft = await generate('kaoyan-review', '整理本次错题');
    assert.equal(privateDraft.content1, '整理本次错题');
    assert.equal(requests.length, 5);
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({ passed: true, environment: 'Edge with synthetic model and account endpoints',
      checks: ['web consent', 'weekly plan and allowlisted study counts', 'review without study data',
        'editable conversation draft and destination fencing', 'manual plaza visibility confirmation',
        'account change rejection', 'Windows private assistant protocol'] }));
  } catch (error) {
    const state = await page?.evaluate(() => ({
      url: location.href, title: document.title,
      auth: document.querySelector('#auth-panel')?.className,
      conversations: document.querySelectorAll('#conversation-list button').length,
      chatStatus: document.querySelector('#chat-status')?.textContent,
      toast: document.querySelector('#toast-region')?.textContent,
      hasDraft: sessionStorage.getItem('nebula:xiaoyan:handoff:v1') !== null,
    })).catch(() => null);
    console.error(JSON.stringify({ state, errors }));
    throw error;
  } finally {
    await browser.close();
  }
}

run().catch(error => { console.error(error); process.exitCode = 1; });
