/* 浏览器测试使用真实前端 + 可控协议替身，专门制造 ACK 丢失和断线。
 * 它不替代真实服务器/数据库黑盒测试，也不下载浏览器或第三方依赖。
 * 设置 PLAYWRIGHT_MODULE 与 CHAT_TEST_BROWSER 指向已经安装的工具。
 */
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../../..');
const messages = [];
const sockets = new Set();
const events = new Set();
const commands = [];
const errors = [];
let offline = false;
let sendMode = 'normal';
const members = [
  [1, 'owner'], [2, 'administrator'], [3, 'member'], [4, 'administrator'],
].map(([userId, role]) => ({ userId, role, username: `user${userId}`,
  displayName: `用户${userId}`, lastReadSequence: 0, lastDeliveredSequence: 0 }));
function add(senderId = 3, key = `seed-${messages.length + 1}`, body = `消息 ${messages.length + 1}`) {
  const sequence = messages.length + 1;
  const message = { id: sequence, sequence, conversationId: 1, senderId,
    clientMessageId: key, body, state: 'active', createdAt: Date.now(), attachments: [] };
  messages.push(message);
  return message;
}
for (let i = 1; i <= 231; i++) add(i === 231 ? 1 : i === 230 ? 2 : i === 228 ? 4 : 3);
function summary() { return { id: 1, kind: 'group', title: '可靠性测试群', state: 'active',
  createdBy: 1, nextSequence: messages.length + 1, unreadCount: 0 }; }
function broadcast(message) { sockets.forEach(socket => socket.send(JSON.stringify(message))); }
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const userId = Number(/user=(\d+)/.exec(req.headers.cookie || '')?.[1] || 2);
  const json = value => { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(value)); };
  if (url.pathname === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store' });
    res.write(': connected\n\n'); events.add(res); req.on('close', () => events.delete(res)); return;
  }
  if (url.pathname === '/api/me') return json({ user: { id: userId, username: `user${userId}`,
    displayName: `用户${userId}` }, csrfToken: 'test-csrf' });
  if (url.pathname === '/api/conversations') return json([summary()]);
  if (url.pathname.endsWith('/members')) return json(members);
  if (url.pathname.endsWith('/messages/by-client-id')) return json(messages.find(message =>
    message.senderId === userId && message.clientMessageId === url.searchParams.get('clientMessageId')) || null);
  if (url.pathname.endsWith('/messages')) {
    const limit = Number(url.searchParams.get('limit') || 100);
    if (url.searchParams.has('before')) {
      const before = Number(url.searchParams.get('before'));
      return json(messages.filter(message => !before || message.sequence < before).slice(-limit));
    }
    return json(messages.filter(message => message.sequence > Number(url.searchParams.get('after') || 0)).slice(0, limit));
  }
  if (url.pathname.startsWith('/api/')) return json([]);
  const relative = url.pathname === '/phase11/' ? 'index.html' : url.pathname.slice('/phase11/'.length);
  const file = path.join(root, 'frontend/phase11', relative);
  if (!url.pathname.startsWith('/phase11/') || !fs.existsSync(file)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'Content-Type': file.endsWith('.js') ? 'text/javascript' :
    file.endsWith('.css') ? 'text/css' : 'text/html', 'Cache-Control': 'no-store' });
  fs.createReadStream(file).pipe(res);
});

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHAT_TEST_BROWSER });
  try {
    async function open(userId) {
      const context = await browser.newContext({ viewport: { width: 1440, height: 960 } });
      await context.addCookies([{ name: 'user', value: String(userId), url: origin }]);
      const page = await context.newPage();
      page.on('pageerror', error => errors.push(error.message));
      page.on('dialog', dialog => dialog.accept());
      await page.routeWebSocket('**/ws', socket => {
        if (offline) { socket.close(); return; }
        sockets.add(socket);
        socket.onClose(() => sockets.delete(socket));
        socket.onMessage(raw => {
          const command = JSON.parse(String(raw)); commands.push(command);
          if (command.type === 'chat.send') {
            if (sendMode === 'uncommitted') { sendMode = 'normal'; return; }
            const message = messages.find(message => message.senderId === userId &&
              message.clientMessageId === command.id) || add(userId, command.id, command.content);
            if (sendMode === 'committed') { sendMode = 'normal'; return; }
            socket.send(JSON.stringify({ type: 'chat.accepted', replyTo: command.id,
              conversationId: 1, id: String(message.id), sequence: message.sequence, status: 'persisted' }));
            broadcast({ ...message, type: 'chat.message' });
          } else if (command.type === 'chat.recall') {
            const message = messages.find(message => String(message.id) === command.replyTo);
            message.state = 'recalled'; message.body = '';
            broadcast({ ...message, type: 'chat.message-recalled' });
            socket.send(JSON.stringify({ type: 'chat.recall.accepted', replyTo: command.id,
              conversationId: 1, status: 'recalled' }));
          }
        });
      });
      await page.goto(`${origin}/phase11/`);
      await page.locator('#conversation-list button').first().click();
      await page.waitForFunction(() => document.querySelectorAll('#message-list .message[data-message-id]').length === 100);
      return { context, page };
    }
    const { context, page } = await open(2);
    await page.locator('.history-load-button').click();
    await page.waitForFunction(() => document.querySelectorAll('#message-list .message[data-message-id]').length === 200);
    await page.locator('.history-load-button').click();
    await page.waitForFunction(() => document.querySelectorAll('#message-list .message[data-message-id]').length === 231);
    assert.equal(await page.locator('.history-load-button').textContent(), '已到最早消息');
    assert.equal(await page.locator('[data-message-id="229"] .message-recall').textContent(), '管理撤回');
    assert.equal(await page.locator('[data-message-id="231"] .message-recall').count(), 0);
    assert.equal(await page.locator('[data-message-id="228"] .message-recall').count(), 0);
    await page.locator('[data-message-id="229"] .message-recall').click();
    await page.waitForFunction(() => document.querySelector('[data-message-id="229"]')?.classList.contains('message-recalled'));
    console.log('PASS browser latest/older pagination and administrator recall');

    // 连续丢失 129 条；重连前再撤回一条旧消息。回放恢复必须跨越两页。
    offline = true;
    for (const socket of [...sockets]) socket.close();
    for (const stream of [...events]) stream.end();
    while (messages.length < 360) add();
    messages[226].state = 'recalled'; messages[226].body = '';
    offline = false;
    await page.waitForFunction(() => document.querySelectorAll('#message-list .message[data-message-id]').length === 360,
      null, { timeout: 12000 });
    assert.equal(await page.locator('[data-message-id="227"] .message-bubble').textContent(), '这条消息已被撤回');
    const sequences = await page.locator('#message-list .message[data-message-id]').evaluateAll(items => items.map(item => Number(item.dataset.sequence)));
    assert.deepEqual(sequences, Array.from({ length: 360 }, (_, i) => i + 1));
    console.log('PASS browser reconnect catches every page and missed recall');

    await page.waitForFunction(() => document.querySelector('#header-ws-state')?.textContent.includes('已连接'));

    async function send(text) {
      await page.locator('#message-form textarea').fill(text);
      await page.locator('#message-form button[type="submit"]').click();
    }
    sendMode = 'committed'; await send('ACK 已提交但丢失');
    await page.waitForFunction(() => [...document.querySelectorAll('.message[data-message-id] .message-bubble')]
      .some(item => item.textContent === 'ACK 已提交但丢失'), null, { timeout: 9000 });
    assert.equal(commands.filter(command => command.content === 'ACK 已提交但丢失').length, 1);
    assert.equal(messages.filter(message => message.body === 'ACK 已提交但丢失').length, 1);
    sendMode = 'uncommitted'; await send('ACK 未提交需重试');
    try {
      await page.waitForFunction(() => [...document.querySelectorAll('.message[data-message-id] .message-bubble')]
        .some(item => item.textContent === 'ACK 未提交需重试'), null, { timeout: 12000 });
    } catch (error) {
      console.error({ sends: commands.filter(command => command.type === 'chat.send'),
        pending: await page.evaluate(() => sessionStorage.getItem('phase11:pending:2')),
        status: await page.locator('#chat-status').textContent(), errors });
      throw error;
    }
    const retries = commands.filter(command => command.content === 'ACK 未提交需重试');
    assert.equal(retries.length, 2); assert.equal(retries[0].id, retries[1].id);
    assert.equal(messages.filter(message => message.body === 'ACK 未提交需重试').length, 1);
    assert.equal(await page.locator('.message-delivery.sending').count(), 0);
    console.log('PASS browser lost ACK reconciliation and fixed-ID retry');
    const owner = await open(1);
    assert.equal(await owner.page.locator('[data-message-id="361"] .message-recall').textContent(), '管理撤回');
    const member = await open(3);
    assert.equal(await member.page.locator('[data-message-id="361"] .message-recall').count(), 0);
    sendMode = 'committed'; await send('刷新后核对原编号');
    await page.waitForFunction(() => JSON.parse(sessionStorage.getItem('phase11:pending:2') || '[]').length === 1);
    await page.reload();
    await page.locator('#conversation-list button').first().click();
    await page.waitForFunction(() => [...document.querySelectorAll('.message[data-message-id] .message-bubble')]
      .some(item => item.textContent === '刷新后核对原编号'));
    assert.equal(commands.filter(command => command.content === '刷新后核对原编号').length, 1);
    await page.waitForFunction(() => sessionStorage.getItem('phase11:pending:2') === '[]');
    offline = true;
    for (const socket of [...sockets]) socket.close();
    await page.waitForFunction(() => document.querySelector('#header-ws-state')?.textContent.includes('重连中'));
    await send('等待本机连接恢复');
    assert.equal(commands.filter(command => command.content === '等待本机连接恢复').length, 0);
    offline = false;
    await page.waitForFunction(() => [...document.querySelectorAll('.message[data-message-id] .message-bubble')]
      .some(item => item.textContent === '等待本机连接恢复'), null, { timeout: 9000 });
    assert.equal(commands.filter(command => command.content === '等待本机连接恢复').length, 1);
    console.log('PASS browser pending reload and sender offline queue recovery');
    assert.deepEqual(errors, []);
    await page.screenshot({ path: path.join(root, 'build-phase27/chat-browser.png'), fullPage: true });
    await context.close(); await owner.context.close(); await member.context.close();
    console.log('PASS browser owner/member permission entry and no JavaScript errors');
  } finally {
    await browser.close(); events.forEach(stream => stream.end());
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.closeAllConnections(); server.close(); });
