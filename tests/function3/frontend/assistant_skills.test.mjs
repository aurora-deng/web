import assert from 'node:assert/strict';
import test from 'node:test';
import { skillById, skillMessages, tools } from '../../../frontend/phase11/shared/AssistantSkills/SkillCatalog.js';
import { readSkillTool } from '../../../frontend/phase11/shared/AssistantSkills/SkillTools.js';
import {
  clearChatDraft, finishSkillDraft, matchesChatDraft, pendingSkillDraft, prefillChatDraft, prefillPlazaDraft, stageSkillDraft,
} from '../../../frontend/phase11/shared/AssistantSkills/DraftHandoff.js';

const origin = 'https://skills.example.test';
const id = 'a'.repeat(32);
const owner = '7';
const day = '2026-11-24';
const statistics = { day, todayPlanned: 2, todayCompleted: 1, overdue: 3, nextSevenDays: 4 };
const signal = new AbortController().signal;

function tabStorage() {
  const items = new Map();
  return { getItem: key => items.get(key) ?? null, setItem: (key, value) => items.set(key, value), removeItem: key => items.delete(key) };
}

test('study skill sends only the entered brief and allowed counters, one pair of bounded messages', () => {
  const progress = { ...statistics, accountId: owner, title: 'private task title', attachments: ['untrusted'] };
  const plan = skillMessages(skillById('kaoyan-week-plan'), '复习网络与英语', progress);
  assert.deepEqual(plan.map(message => message.role), ['system', 'user']);
  assert.match(plan[1].content, /"overdue":3/);
  for (const forbidden of ['accountId', 'private task title', 'attachments']) assert(!plan[1].content.includes(forbidden));
  assert.equal(skillMessages(skillById('chat-draft'), '给同学草拟回复', progress)[1].content, '给同学草拟回复');
  assert.throws(() => skillMessages({ id: 'chat-draft' }, 'x'), /请选择技能/);
  assert.throws(() => skillMessages(skillById('kaoyan-review'), '字'.repeat(1501)), /1500/);
});

test('study tool verifies the scoped session before fetching the task-count projection', async () => {
  const fetches = [];
  const scope = 'b'.repeat(32);
  const binding = { accountId: owner, scope, url: `/api/study/${scope}/` };
  const context = { owner, day, signal, fetcher: async (path, options) => {
    fetches.push(path);
    assert.equal(options.credentials, 'same-origin');
    return { ok: true, json: async () => path === '/api/study/session' ? binding : { ...statistics, accountId: owner, note: 'private' } };
  } };
  assert.deepEqual(await readSkillTool(tools.study, context), statistics);
  assert.deepEqual(fetches, ['/api/study/session', binding.url + 'api/assistant-summary?day=' + day]);
  binding.url = 'https://other.example.test/api/study/';
  await assert.rejects(readSkillTool(tools.study, context), /不匹配/);
  assert.equal(fetches.length, 3);
  await assert.rejects(readSkillTool('plaza.publish', context), /未登记/);
  assert.equal(fetches.length, 3);
});

test('read tools recheck the account and return safe options without history or model instructions', async () => {
  const api = { request: async ({ path, method }) => {
    assert.equal(method, 'GET');
    if (path === '/api/me') return { user: { id: owner } };
    assert.equal(path, '/api/conversations');
    return [{ id: '12', displayTitle: '同学', kind: 'direct', latest: 'private message' },
      { id: '9007199254740993', title: 'unsupported in the existing chat composer' }];
  } };
  assert.deepEqual(await readSkillTool(tools.chat, { api, owner, signal }), [{ id: '12', label: '会话 · 同学' }]);
  let reads = 0;
  api.request = async ({ path }) => path === '/api/me' ? { user: { id: ++reads === 1 ? owner : '8' } } : [{ id: '12', name: 'other account' }];
  await assert.rejects(readSkillTool(tools.plaza, { api, owner, signal }), /账号已变化/);
});

test('handoff keeps the text out of the URL and rejects expired, cross-account and already-consumed drafts', () => {
  const storage = tabStorage();
  const input = { owner, destination: 'chat', target: '12', body: '这是一条私信' };
  const url = new URL(stageSkillDraft(input, { storage, id, now: 1_000 }), origin);
  assert(!url.href.includes(input.body));
  assert.deepEqual(pendingSkillDraft('chat', owner, url, { storage, now: 1_100 }),
    { ...input, id, createdAt: 1_000 });
  assert.equal(pendingSkillDraft('chat', '8', url, { storage, now: 1_100 }), null);
  assert.equal(pendingSkillDraft('chat', owner, url, { storage, now: 1_100 }), null);
  stageSkillDraft(input, { storage, id, now: 1_000 });
  assert.equal(pendingSkillDraft('chat', owner, url, { storage, now: 601_001 }), null);
  stageSkillDraft(input, { storage, id, now: 1_000 });
  let replaced;
  finishSkillDraft({ id }, { storage, locationUrl: url.href, historyApi: { replaceState(_, __, next) { replaced = next; } } });
  assert.equal(replaced.href, origin + '/phase11/');
  assert.equal(pendingSkillDraft('chat', owner, url, { storage, now: 1_100 }), null);
});

test('chat stays bound to the selected recipient and plaza prefill never checks public consent', () => {
  const input = { value: '已有内容' };
  const draft = { owner, target: '12', body: '需要先审核的正文' };
  assert.equal(prefillChatDraft(input, draft), false);
  input.value = '';
  assert.equal(prefillChatDraft(input, draft), true);
  assert.equal(matchesChatDraft(input, owner, '12'), true);
  assert.equal(matchesChatDraft(input, owner, '13'), false);
  assert.equal(matchesChatDraft(input, '8', '12'), false);
  clearChatDraft(input);
  assert.equal(input.value, '');
  assert.equal(matchesChatDraft(input, owner, '13'), true);
  const form = { elements: {
    body: { value: '' }, categoryId: { value: '' },
    destination: { value: 'profile', dispatchEvent: event => assert.equal(event.type, 'change') },
    publicConfirmed: { checked: true },
  } };
  assert.equal(prefillPlazaDraft(form, draft, [{ id: '4' }]), false);
  assert.equal(prefillPlazaDraft(form, draft, [{ id: '12' }]), true);
  assert.deepEqual([form.elements.body.value, form.elements.categoryId.value, form.elements.destination.value,
    form.elements.publicConfirmed.checked], [draft.body, '12', 'tree', false]);
});
