import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

// 用标准 Node 测试 ES Module，不需要安装 npm 包或改变项目的模块配置。
async function moduleAt(path) {
  const source = await readFile(new URL(path, import.meta.url), 'utf8');
  return import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
}
const { ChatHistory, canRecallMessage } = await moduleAt('../../../frontend/phase11/chat/ChatHistory.js');
const { ReliableSender } = await moduleAt('../../../frontend/phase11/chat/ReliableSender.js');
const message = (sequence, extra = {}) => ({ id: sequence, conversationId: 1,
  sequence, senderId: 2, clientMessageId: `key-${sequence}`, body: `body-${sequence}`,
  createdAt: 1000, state: 'active', ...extra });

function database(size) {
  const records = Array.from({ length: size }, (_, i) => message(i + 1));
  const fetch = async (id, query) => query.before !== undefined
    ? records.filter(m => !query.before || m.sequence < query.before).slice(-query.limit)
    : records.filter(m => m.sequence > query.after).slice(0, query.limit);
  return { records, fetch };
}

test('最新页与两页旧历史无遗漏、无重叠，结果保持升序', async () => {
  const db = database(231);
  const history = new ChatHistory(db.fetch);
  await history.open(1);
  assert.equal(history.oldest, 132);
  assert.equal(history.cursor, 231);
  await history.older();
  assert.equal(history.oldest, 32);
  await history.older();
  assert.deepEqual(history.messages.map(m => m.sequence), Array.from({ length: 231 }, (_, i) => i + 1));
  assert.equal(history.hasOlder, false);
});

test('较高乱序广播不跳过缺口；重连分页补齐并刷新旧消息撤回', async () => {
  const db = database(110);
  const history = new ChatHistory(db.fetch, () => {}, 25);
  await history.open(1);
  db.records[99] = message(100, { state: 'recalled', body: '' });
  for (let i = 111; i <= 235; ++i) db.records.push(message(i));
  history.ingest(message(235));
  assert.equal(history.cursor, 110);
  await history.synchronize(235, { refreshLoaded: true });
  assert.equal(history.cursor, 235);
  assert.equal(history.records.get('100').state, 'recalled');
  assert.deepEqual(history.messages.map(m => m.sequence), Array.from({ length: 150 }, (_, i) => i + 86));
});

test('撤回终态不能被较晚到达的旧 HTTP 响应覆盖', async () => {
  const history = new ChatHistory(database(3).fetch);
  await history.open(1);
  history.ingest(message(3, { state: 'recalled', body: '' }));
  history.ingest(message(3));
  assert.equal(history.records.get('3').body, '');
});

test('切换会话后忽略上一会话迟到页面', async () => {
  let release;
  const history = new ChatHistory((id) => id === 1
    ? new Promise(resolve => { release = resolve; })
    : Promise.resolve([message(1, { conversationId: 2, id: 20 })]));
  const previous = history.open(1);
  await history.open(2);
  release([message(1)]);
  await previous;
  assert.equal(history.conversationId, 2);
  assert.equal(history.messages[0].id, 20);
});

test('空会话随后出现新消息也能恢复；无进展页面不会无限循环', async () => {
  const db = database(0);
  const history = new ChatHistory(db.fetch);
  await history.open(1);
  db.records.push(message(1));
  await history.synchronize(1);
  assert.equal(history.cursor, 1);
  await assert.rejects(history.synchronize(2), /未完成/);
});

test('合并同步目标；同一旧页同时点击只请求一次', async () => {
  const db = database(101);
  let requests = 0;
  const history = new ChatHistory(async (...args) => { requests++; return db.fetch(...args); });
  await history.open(1);
  await Promise.all([history.older(), history.older()]);
  assert.equal(requests, 2);
  db.records.push(message(102), message(103));
  await Promise.all([history.synchronize(102), history.synchronize(103)]);
  assert.equal(history.cursor, 103);
});

function clock() {
  let next = 0;
  const jobs = new Map();
  return { jobs, schedule: (fn, delay) => { jobs.set(++next, { fn, delay }); return next; },
    cancel: id => jobs.delete(id), async tick() {
      const [id, job] = jobs.entries().next().value || [];
      assert.ok(job, 'no timer'); jobs.delete(id); await job.fn();
    } };
}
function senderFixture(verify = async () => null) {
  const time = clock();
  const sent = [];
  const saved = [];
  let online = true;
  const sender = new ReliableSender({ send: p => { if (!online) return false; sent.push(p); return true; },
    verify, changed: () => {}, persisted: (entry, m) => saved.push(m),
    schedule: time.schedule, cancel: time.cancel });
  return { sender, time, sent, saved, offline: () => { online = false; sender.disconnected(); },
    online: () => { online = true; return sender.connected(); } };
}
const payload = { v: 1, type: 'chat.send', id: 'same-key', conversationId: 1, content: 'hello' };

test('ACK 丢失但已落库：查询确认，不再发送', async () => {
  const f = senderFixture(async () => message(1, { clientMessageId: payload.id }));
  f.sender.submit(payload);
  await f.time.tick();
  assert.equal(f.sent.length, 1);
  assert.equal(f.saved.length, 1);
  assert.equal(f.sender.entries.size, 0);
});

test('未落库的 ACK 超时：原载荷有限重试，保留未知结果并允许手动重试', async () => {
  const f = senderFixture();
  f.sender.submit(payload);
  for (let i = 0; i < 5; i++) await f.time.tick();
  assert.equal(f.sent.length, 3);
  f.sent.forEach(p => assert.deepEqual(p, payload));
  assert.equal(f.sender.entries.get(payload.id).status, 'unknown');
  assert.equal(f.time.jobs.size, 0);
  await f.sender.retry(payload.id);
  await f.time.tick();
  assert.equal(f.sent.length, 4);
  assert.equal(f.sent[3].id, payload.id);
});

test('断线和页面恢复保留原幂等键；后台会话回执也能结束队列', async () => {
  const f = senderFixture();
  f.sender.submit(payload);
  f.offline();
  assert.equal(f.time.jobs.size, 0);
  const snapshot = f.sender.snapshot();
  const recovered = senderFixture();
  recovered.sender.restore(snapshot);
  await recovered.online();
  await recovered.time.tick();
  assert.equal(recovered.sent[0].id, payload.id);
  recovered.sender.confirm({ replyTo: payload.id, id: 10, conversationId: 1, sequence: 10 });
  assert.equal(recovered.sender.entries.size, 0);
  assert.equal(recovered.time.jobs.size, 0);
});

test('广播先到、accepted 后到不会重复收敛或丢失完整消息', () => {
  const f = senderFixture();
  f.sender.submit(payload);
  f.sender.confirmMessage(message(1, { clientMessageId: payload.id }));
  f.sender.confirm({ replyTo: payload.id, id: 1, sequence: 1, conversationId: 1 });
  assert.equal(f.saved.length, 1);
  assert.equal(f.saved[0].createdAt, 1000);
});

test('查询失败仍为未知；显式服务器拒绝先核对再显示失败', async () => {
  const f = senderFixture(async () => { throw Error('offline'); });
  f.sender.submit(payload);
  await f.time.tick();
  assert.equal(f.sender.entries.get(payload.id).status, 'unknown');
  assert.equal(f.sent.length, 1);
  const refused = senderFixture();
  refused.sender.submit(payload);
  await refused.sender.reject(payload.id, 'muted');
  assert.equal(refused.sender.entries.get(payload.id).status, 'failed');
  assert.equal(refused.time.jobs.size, 0);
});

test('ACK 校验会话、队列有上限、退出后迟到查询不再写状态', async () => {
  let finish;
  const f = senderFixture(() => new Promise(resolve => { finish = resolve; }));
  f.sender.submit(payload);
  f.sender.confirm({ replyTo: payload.id, id: 1, sequence: 1, conversationId: 2 });
  assert.equal(f.sender.entries.size, 1);
  for (let i = 1; i < 32; ++i) f.sender.submit({ ...payload, id: `key-${i}` });
  assert.throws(() => f.sender.submit({ ...payload, id: 'full' }), /32/);
  const pending = f.time.tick();
  f.sender.clear();
  finish(message(1, { clientMessageId: payload.id }));
  await pending;
  assert.equal(f.saved.length, 0);
});

test('群管理撤回按钮符合角色、时间窗和撤回终态', () => {
  const members = [{ userId: 1, role: 'owner' }, { userId: 2, role: 'administrator' },
    { userId: 3, role: 'member' }, { userId: 4, role: 'administrator' }];
  const group = { kind: 'group', state: 'active' };
  const m = message(1, { senderId: 3 });
  assert.equal(canRecallMessage(m, 2, group, members, 2000), true);
  assert.equal(canRecallMessage({ ...m, senderId: 1 }, 2, group, members, 2000), false);
  assert.equal(canRecallMessage({ ...m, senderId: 4 }, 2, group, members, 2000), false);
  assert.equal(canRecallMessage({ ...m, senderId: 2 }, 1, group, members, 2000), true);
  assert.equal(canRecallMessage({ ...m, senderId: 1 }, 3, group, members, 2000), false);
  assert.equal(canRecallMessage(m, 2, group, members, 122000), false);
  assert.equal(canRecallMessage(m, 2, group, members, 999), false);
  assert.equal(canRecallMessage({ ...m, state: 'recalled' }, 2, group, members, 2000), false);
  assert.equal(canRecallMessage(m, 2, { kind: 'direct' }, members, 2000), false);
});
