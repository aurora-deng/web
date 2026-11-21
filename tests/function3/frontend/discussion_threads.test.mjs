import test from 'node:test';
import assert from 'node:assert/strict';
import { groupDiscussion } from '../../../frontend/phase11/shared/DiscussionThreads/DiscussionThreads.js';

test('older pages reconnect replies to their main comment without flattening another thread', () => {
  const root = { id: '9007199254740993', parentReplyId: '0' };
  const child = { id: '9007199254740994', parentReplyId: root.id };
  const nested = { id: '9007199254740995', parentReplyId: child.id };
  const other = { id: '9007199254740996', parentReplyId: '0' };
  assert.deepEqual(groupDiscussion([nested, child]), [{ root: undefined, replies: [nested, child] }]);
  assert.deepEqual(groupDiscussion([other, nested, child, root]), [
    { root: other, replies: [] }, { root, replies: [nested, child] },
  ]);
});
