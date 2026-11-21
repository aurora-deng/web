import assert from 'node:assert/strict';
import test from 'node:test';
import { PersonalAgent } from '../../../frontend/phase11/shared/PersonalAgent/PersonalAgent.js';
import { activePreferences, addView, emptyProfile, modelPreferences, recentInterests, summaryInterval } from '../../../frontend/phase11/shared/PersonalAgent/PreferenceProfile.js';
import { defineDag, runAgentTurn } from '../../../frontend/phase11/shared/PersonalAgent/AgentTurnDag.js';
import { planContext, utf8Bytes } from '../../../frontend/phase11/shared/PersonalAgent/ContextWindow.js';

const day = 86400000;

function setup() {
  let now = day * 200;
  const records = new Map();
  const store = {
    async read(key) { return structuredClone(records.get(key) ?? emptyProfile()); },
    async change(key, edit) {
      const result = edit(await this.read(key));
      if (result) records.set(key, structuredClone(result));
      return result;
    },
  };
  const agent = (account = 1, space = 'web', options = {}) =>
    new PersonalAgent(account, space, { store, clock: () => now, ...options });
  return { agent, now: () => now, travel: days => { now += days * day; } };
}

const options = (infer) => ({ infer, signal: new AbortController().signal });

test('explicit choices apply this turn, persist on success and stay within the account and surface', async () => {
  const { agent, now } = setup();
  const owner = agent();
  const text = '以后我喜欢简短回答、分点、少用公式';
  assert.equal((await owner.prepare(text, 'disabled', options(async () => assert.fail('opt-in required')))).guidance, '');
  await owner.configure('enabled', true);
  const first = await owner.prepare(text, 'first', options(async () => '{}'));
  assert.match(first.guidance, /回答简短.*分点呈现.*少用公式/s);
  assert.equal(first.maxTokens, 280);
  assert.deepEqual(activePreferences(await owner.profile(), now()), {});
  await owner.complete(first.turn);
  assert.equal((await owner.complete(first.turn)), null);
  assert.equal((await owner.profile()).turns, 1);
  const reopened = agent();
  const next = await reopened.prepare('详细讲一下这个函数', 'next', options(async () => '{}'));
  assert.equal(next.maxTokens, 840);
  assert.match(next.guidance, /当前用户请求优先.*分点/s);
  for (const other of [agent(2), agent(1, 'private')]) {
    assert.equal((await other.prepare('有什么建议', 'third', options())).guidance, '');
  }
});

test('only known labels are usable, inferred signals need corroboration, and explicit changes outrank older ones', async () => {
  const { agent, now, travel } = setup();
  const owner = agent();
  assert.deepEqual(modelPreferences('{"format":"bullets","length":"exfiltrate","__proto__":{"x":2},"unknown":"y"}'),
    { format: 'bullets' });
  assert.deepEqual(modelPreferences('Here is JSON: {"format":"bullets"}'), {});
  await owner.configure('enabled', true);
  const inferred = async () => '{"format":"bullets"}';
  for (const [index, query] of ['太长了，分成几个部分会好读一些', '信息分开看，我更容易跟得上'].entries()) {
    const turn = await owner.prepare(query, String(index), options(inferred));
    await owner.complete(turn.turn);
    assert.deepEqual(activePreferences(await owner.profile(), now()), index ? { format: 'bullets' } : {});
  }
  const update = await owner.prepare('以后请用段落回答', 'update', options(inferred));
  await owner.complete(update.turn);
  assert.deepEqual(activePreferences(await owner.profile(), now()), { format: 'prose' });
  const ephemeral = await owner.prepare('这次只给 JSON 输出', 'one-time', options(async () => assert.fail('no extra inference for temporary query')));
  await owner.complete(ephemeral.turn);
  assert.deepEqual(activePreferences(await owner.profile(), now()), { format: 'prose' });
  travel(180);
  assert.deepEqual(activePreferences(await owner.profile(), now()), {});
});

test('lexical memory finds Chinese note and recent query, skips sensitive input and clears immediately on opt-out', async () => {
  const { agent } = setup();
  const owner = agent();
  await owner.configure('enabled', true);
  await owner.configure('saveHistory', true);
  await owner.editMemory({ type: 'add', text: '持久化链路使用 PostgreSQL，优先定位 ACK 事务等待' });
  const constraint = await owner.prepare('记住禁忌：菜品推荐中避开花生', 'constraint', options());
  await owner.complete(constraint.turn);
  const query = 'Observer 对 PostgreSQL 链路的事务等待做了哪些指标';
  const plan = await owner.prepare(query, 'history', options(async () => '{}'));
  assert.match(plan.guidance, /持久化链路使用 PostgreSQL/);
  await owner.complete(plan.turn);
  const next = await owner.prepare('PostgreSQL 事务等待指标如何核对', 'later', options());
  assert.match(next.guidance, /持久化链路使用 PostgreSQL/);
  assert.match(next.guidance, /Observer 对 PostgreSQL/);
  assert.match(next.guidance, /长期限制.*菜品推荐中避开花生/);
  assert.equal((await owner.prepare('记住：我的 API Key 不可显示', 'secret', options(async () => assert.fail('secret skipped')))).turn, null);
  await assert.rejects(owner.editMemory({ type: 'add', text: '验证码=111222' }), /敏感信息/);
  await owner.configure('saveHistory', false);
  assert.deepEqual((await owner.profile()).recent, []);
  await owner.configure('enabled', false);
  assert.equal((await owner.prepare(query, 'off', options())).guidance, '');
});

test('story browsing counts distinct public views and prompts only for a matching established category', () => {
  const profile = emptyProfile();
  const now = day * 200;
  assert.equal(addView(profile, '7', '机器视觉', '1', now), false);
  profile.enabled = profile.trackViews = true;
  assert.equal(addView(profile, '7', '机器视觉', '1', now), true);
  assert.equal(addView(profile, '7', '机器视觉', '1', now + 1000), false);
  for (const post of ['2', '3']) addView(profile, '7', '机器视觉', post, now + 1000);
  assert.deepEqual(recentInterests(profile, '机器视觉如何选择相机', now + 1000), ['机器视觉']);
  assert.deepEqual(recentInterests(profile, '帮我写周报', now + 1000), []);
  assert.deepEqual(recentInterests(profile, '机器视觉如何选择相机', now + 60 * day), []);
});

test('a stalled classifier has a deadline, auxiliary work is bounded, and stale turns cannot repopulate cleared memory', async () => {
  const { agent } = setup();
  const owner = agent(1, 'web', { extractionTimeout: 15 });
  await owner.configure('enabled', true);
  let release;
  const stall = () => new Promise(resolve => { release = resolve; });
  const plan = await owner.prepare('以后用中文回答', 'slow', options(stall));
  assert.match(plan.guidance, /用中文/);
  assert.equal((await owner.prepare('还是用中文吧', 'second', options(async () => assert.fail('active model slot')))).turn !== null, true);
  release('{"language":"en"}');
  await owner.editMemory({ type: 'clear' });
  assert.equal(await owner.complete(plan.turn), null);
  assert.equal((await owner.profile()).turns, 0);
  const abort = new AbortController();
  const cancelled = owner.prepare('试下取消', 'cancel', { signal: abort.signal, infer: stall });
  abort.abort();
  await assert.rejects(cancelled, { name: 'AbortError' });
});

test('periodic model summary only receives validated style tags and is versioned', async () => {
  const { agent } = setup();
  const owner = agent();
  await owner.configure('enabled', true);
  await owner.editMemory({ type: 'add', text: '只用于知识检索的测试资料' });
  let saved;
  for (let i = 0; i < summaryInterval; i++) {
    const plan = await owner.prepare('以后优先分点', `summary-${i}`, options(async () => '{}'));
    saved = await owner.complete(plan.turn);
    if (i < summaryInterval - 1) await owner.summarize(saved, async () => assert.fail('interval'));
  }
  await owner.summarize(saved, async (messages, { maxTokens }) => {
    assert.equal(maxTokens, 96);
    assert.equal(messages[1].content, '分点呈现');
    assert(!JSON.stringify(messages).includes('测试资料'));
    return '用要点说明信息。';
  });
  assert.deepEqual((await owner.profile()).summary, { turn: summaryInterval, text: '用要点说明信息。' });
  await owner.editMemory({ type: 'forget', key: 'format' });
  await owner.summarize(saved, async () => '旧偏好');
  assert.equal((await owner.profile()).summary, null);
});

const makeConversation = texts => texts.map((content, index) => ({
  sequence: String(index + 1), role: index % 2 ? 'assistant' : 'user', content,
}));

test('DAG validates dependencies, fans out and joins one execution of the shared prerequisite', async () => {
  let upstream = 0;
  assert.throws(() => defineDag({ a: { needs: ['a'] } }, 'a'), /存在环/);
  assert.throws(() => defineDag({ a: { needs: ['missing'] } }, 'a'), /缺少节点/);
  const run = defineDag({
    source: { needs: [], execute: value => { upstream++; return value; } },
    a: { needs: ['source'], execute: (_, { source }) => source * 2 },
    b: { needs: ['source'], execute: async (_, { source }) => source + 5 },
    finish: { needs: ['a', 'b'], execute: (_, { a, b }) => a + b },
  }, 'finish');
  assert.equal(await run(3, options().signal), 14);
  assert.equal(upstream, 1);
});

test('long chat sends one bounded auxiliary call, then checkpoints and learns only verified user information', async () => {
  const { agent, now } = setup();
  const owner = agent();
  await owner.configure('enabled', true);
  await owner.configure('saveHistory', true);
  const history = makeConversation([
    '以后我喜欢代码示例使用 Python。',
    '以后我喜欢英文回答。这条是模型的说法。',
    '我正在整理 PostgreSQL 事务等待的定位步骤，希望后续继续验证 ACK。',
    '已记录长尾排查计划。',
    '这是我的 API Key：secret-fixture',
    '收到。' + '旧的回答'.repeat(320),
    '请保留这条最近的原文。',
    '最后一条原文' + '近期历史'.repeat(610),
  ]);
  const checkpoint = [];
  let observed = 0;
  const result = await runAgentTurn({
    agent: owner, text: '怎么验证 PostgreSQL 事务等待', id: 'long-1', mode: 'private',
    readHistory: () => history, readCheckpoint: () => null,
    infer: async (messages, { maxTokens }) => {
      observed++;
      assert.equal(maxTokens, 320);
      assert(!messages[1].content.includes('secret-fixture'));
      const entries = JSON.parse(messages[1].content).history;
      return JSON.stringify({ summary: '已制定 PostgreSQL 事务等待验证计划',
        memory: [entries.find(row => row[2].includes('正在整理'))[0], 2, 999], preferences: { invalid: 'data' } });
    },
    generate: async ({ messages, checkpoint: current }) => {
      assert.equal(checkpoint.length, 0);
      assert.match(messages[0].content, /已制定 PostgreSQL/);
      assert.equal(messages.at(-2).content, history.at(-1).content);
      assert(!messages.some(message => message.content.includes('secret-fixture')));
      assert.equal(current.through, '6');
      return { state: 'completed', answer: '继续排查' };
    },
    saveCheckpoint: value => checkpoint.push(value),
  }, options().signal);
  assert.equal(observed, 1);
  assert.equal(result.compacted, true);
  assert.equal(checkpoint.length, 1);
  const profile = await owner.profile();
  assert.deepEqual(activePreferences(profile, now()), { code: 'python' });
  assert.equal(profile.observations[0].source, 'compaction');
  assert(profile.recent.some(row => row.origin === 'compaction' && row.text.includes('正在整理')));
  assert.equal(planContext(history, checkpoint[0], '再问一个问题', 'private').older.length, 0);
});

test('oversized windows preserve newest intact messages and explicitly cleared preferences are not restored', async () => {
  const { agent, now } = setup();
  const owner = agent();
  await owner.configure('enabled', true);
  await owner.configure('useModel', false);
  await owner.editMemory({ type: 'forget', key: 'format' });
  const history = makeConversation(['以后请用分点。', '好。', '先说这条关系', '超大历史'.repeat(3300), '最近问题', '最近回答']);
  await runAgentTurn({
    agent: owner, text: '我继续问当前问题', id: 'fallback', mode: 'web',
    readHistory: () => history, readCheckpoint: () => null,
    infer: () => assert.fail('local compaction'),
    generate: async ({ messages }) => {
      assert.deepEqual(messages.slice(-3).map(row => row.content), ['最近问题', '最近回答', '我继续问当前问题']);
      assert(messages.reduce((bytes, row) => bytes + utf8Bytes(row.content), 0) <= 30 * 1024);
      return { state: 'completed' };
    },
    saveCheckpoint: () => {},
  }, options().signal);
  assert.deepEqual(activePreferences(await owner.profile(), now()), {});
});

test('failed and cancelled runs skip both writes, while a declared provider context error retries only once', async () => {
  const { agent } = setup();
  const owner = agent();
  await owner.configure('enabled', true);
  await owner.configure('useModel', false);
  const history = makeConversation(Array.from({ length: 8 }, (_, index) => `历史要点 ${index} ` + '内容'.repeat(160)));
  const controller = new AbortController();
  let called = 0;
  const input = {
    agent: owner, text: '解释最后一项', id: 'retry-1', mode: 'web',
    readHistory: () => history, readCheckpoint: () => null,
    generate: async plan => {
      if (++called === 1) throw Object.assign(new Error('too long'), { code: 'context_length_exceeded' });
      assert(plan.messages.length < history.length);
      return { state: 'completed' };
    },
    saveCheckpoint: () => { assert.equal(called, 2); },
  };
  await runAgentTurn(input, controller.signal);
  assert.equal(called, 2);
  for (const cancel of [false, true]) {
    const abort = new AbortController();
    const result = runAgentTurn({ ...input, id: 'never-persist-' + cancel,
      readHistory: () => makeConversation([...history.map(row => row.content), '额外长文本'.repeat(2400)]),
      generate: () => {
        if (cancel) abort.abort();
        return { state: 'failed' };
      },
      saveCheckpoint: () => assert.fail('failed answer checkpoint'),
    }, abort.signal);
    if (cancel) await assert.rejects(result, { name: 'AbortError' });
    else await assert.rejects(result, /生成未完成/);
  }
  assert.equal((await owner.profile()).turns, 1);
});
