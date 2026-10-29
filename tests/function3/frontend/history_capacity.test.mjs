import test from 'node:test';
import assert from 'node:assert/strict';
import { ChatHistory } from '../../../frontend/phase11/chat/ChatHistory.js';
import { RecoveryBudget, ReconnectBackoff } from '../../../frontend/phase11/shared/RecoveryBudget/RecoveryBudget.js';
import { VirtualMessageList } from '../../../frontend/phase11/chat/VirtualMessageList/VirtualMessageList.js';

const row = sequence => ({ id: sequence, conversationId: 1, sequence, body: `消息${sequence}`, state: 'active' });
function fixture(size) {
  const data = Array.from({ length: size }, (_, i) => row(i + 1));
  return { data, fetch: async (_id, q) => q.before !== undefined
    ? data.filter(m => !q.before || m.sequence < q.before).slice(-q.limit)
    : data.filter(m => m.sequence > q.after).slice(0, q.limit) };
}
test('一万条历史全部可达，缓存上限不阻断前后翻页', async () => {
  const db = fixture(10000), history = new ChatHistory(db.fetch, () => {}, 100, 500);
  await history.open(1);
  while (history.hasOlder) { await history.older(); assert.ok(history.records.size <= 500); }
  assert.equal(history.oldest, 1); assert.equal(history.cursor, 10000);
  while (history.hasNewer) { await history.newer(); assert.ok(history.records.size <= 500); }
  assert.equal(history.latest, 10000); assert.equal(history.lookingBack, false);
});
test('阅读旧页时新消息不挤走窗口，随后可追赶，撤回终态保留', async () => {
  const db = fixture(1000), history = new ChatHistory(db.fetch, () => {}, 100, 300);
  await history.open(1);
  for (let i = 0; i < 5; i++) await history.older();
  const oldest = history.oldest;
  for (let i = 1001; i <= 1100; i++) { db.data.push(row(i)); history.ingest(row(i)); }
  assert.equal(history.oldest, oldest);
  assert.ok(history.records.size <= 300);
  await history.synchronize(1100, { refreshLoaded: true });
  assert.equal(history.cursor, 1100); assert.equal(history.oldest, oldest);
  while (history.hasNewer) await history.newer();
  assert.equal(history.latest, 1100);
});
test('恢复闸机限制并发、速率、排队，切会话取消旧请求', async () => {
  let now = 0, release;
  let startedResolve;
  const started = new Promise(resolve => { startedResolve = resolve; });
  const gate = new RecoveryBudget({ interval: 200, capacity: 2, now: () => now, wait: async ms => { now += ms; } });
  const one = gate.run(() => new Promise(resolve => { release = resolve; startedResolve(); }));
  const controller = new AbortController();
  const two = gate.run(() => assert.fail('已取消的操作不应执行'), controller.signal);
  const rejected = assert.rejects(gate.run(() => {}), /繁忙/);
  controller.abort();
  await started; release();
  await one; await assert.rejects(two, { name: 'AbortError' }); await rejected;
  await gate.run(() => {}); assert.ok(now >= 200); assert.equal(gate.pending, 0);
});
test('重连随机退避有上下界和最大值', () => {
  const backoff = new ReconnectBackoff(() => 0);
  assert.equal(backoff.next(), 800); assert.equal(backoff.next(), 1600);
  for (let i = 0; i < 20; i++) assert.ok(backoff.next() <= 30000);
  backoff.reset(); assert.equal(backoff.next(), 800);
});
test('十万条记录的渲染窗口始终有界，临界滚动位置不返回空窗口', () => {
  const view = Object.create(VirtualMessageList.prototype);
  Object.assign(view, { messages: Array(100000), threshold: 400, maximumRows: 160,
    element: { scrollTop: 5000000, clientHeight: 900 } });
  const prefix = Array.from({ length: 100001 }, (_, i) => i * 100);
  let [a, b] = view.range(prefix);
  assert.ok(a > 0 && b - a <= 160 && b > a);
  view.element.scrollTop = 100000000;
  [a, b] = view.range(prefix);
  assert.equal(b, 100000); assert.equal(a, 99999);
});
