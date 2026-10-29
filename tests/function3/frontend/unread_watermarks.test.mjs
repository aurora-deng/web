import test from 'node:test';
import assert from 'node:assert/strict';
import { UnreadWatermarks } from '../../../frontend/phase11/chat/UnreadWatermarks/UnreadWatermarks.js';
test('10 snapshot plus two concurrent messages is 12; duplicate/old snapshots do not lose data', () => {
  const w = new UnreadWatermarks();
  w.snapshot({ id: 1, nextSequence: 21, lastReadSequence: 10 });
  w.observe(1, 22); w.observe(1, 21); w.observe(1, 22);
  assert.equal(w.snapshot({ id: 1, nextSequence: 21, lastReadSequence: 10 }), 12);
  assert.equal(w.observe(1, 0, 22), 0);
  assert.equal(w.snapshot({ id: 1, nextSequence: 21, lastReadSequence: 10 }), 0);
  assert.equal(w.observe(1, 23), 1);
});
test('read arrives first, exact decimal strings, account reset and removed memberships', () => {
  const w = new UnreadWatermarks();
  w.observe('7', 0, '9007199254740993');
  assert.equal(w.observe('7', '9007199254740995'), 2);
  w.retain(['8']); assert.equal(w.count('7'), 0);
  w.observe('8', 5); w.clear(); assert.equal(w.count('8'), 0);
});
