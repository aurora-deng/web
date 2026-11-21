import assert from 'node:assert/strict';
import test from 'node:test';
import { setTimeout as wait } from 'node:timers/promises';
import { defineDag } from '../../../frontend/phase11/shared/AgentRuntime/DagExecutor.js';
import { createAgentCoordination } from '../../../frontend/phase11/shared/AgentRuntime/AgentCoordination.js';

test('workflow concurrency is shared by runs and run admission has a fixed limit', async () => {
  let active = 0, maximum = 0;
  const work = async () => {
    maximum = Math.max(maximum, ++active);
    await wait(30);
    active--;
    return 1;
  };
  const graph = defineDag({
    first: { needs: [], execute: work },
    second: { needs: [], execute: work },
    third: { needs: [], execute: work },
    join: { needs: ['first', 'second', 'third'], execute: (_, replies) => Object.values(replies).reduce((a, b) => a + b) },
  }, 'join', { concurrency: 2, maxRuns: 2, deadlineMs: 2500 });
  const runs = [graph(), graph()];
  await assert.rejects(graph(), { code: 'dag_busy' });
  assert.deepEqual(await Promise.all(runs), [3, 3]);
  assert.equal(maximum, 2);
});

test('retry keeps an idempotency key, creates a new span and sends only the observer contract', async () => {
  const observations = [], emitted = [];
  const graph = defineDag({
    source: { needs: [], effect: 'read', attempts: 2, backoffMs: 0, execute(input, _, signal, context) {
      observations.push(context);
      signal.throwIfAborted();
      if (context.attempt === 1) throw Object.assign(new Error('secret-message'), { retryable: true });
      return input;
    } },
    persist: { needs: ['source'], effect: 'commit', execute: (_, values) => values.source },
  }, 'persist', { workflow: 'test.v1', deadlineMs: 1000 });
  assert.throws(() => defineDag({ write: { needs: [], effect: 'commit', attempts: 2, execute() {} } }, 'write'), /重试策略/);
  assert.throws(() => defineDag({ write: { needs: [], effect: 'commit', acceptIncomplete: true, execute() {} } }, 'write'), /重试策略/);
  const expectedTrace = 'ab'.repeat(16);
  assert.equal(await graph('secret-input', undefined, { onEvent: row => emitted.push(row), traceId: expectedTrace }), 'secret-input');
  assert.equal(observations.length, 2);
  assert.equal(observations[0].idempotencyKey, observations[1].idempotencyKey);
  assert.notEqual(observations[0].spanId, observations[1].spanId);
  assert(emitted.every(row => row.traceId === expectedTrace));
  assert(!JSON.stringify(emitted).includes('secret'));
  assert.deepEqual(emitted.filter(row => row.fields.nodeId === 'source').map(row => row.fields.state),
    ['queued', 'started', 'failed', 'retry', 'queued', 'started', 'success']);
  assert.equal(emitted.at(-1).fields.state, 'success');
});

test('timeouts block commits and a late worker holds its concurrency slot until it exits', async () => {
  let late, commits = 0;
  const workers = [];
  const graph = defineDag({
    read: { needs: [], effect: 'read', timeoutMs: 18, execute: input => {
      workers.push(input);
      if (input === 'late') return new Promise(resolve => { late = resolve; });
      return input;
    } },
    commit: { needs: ['read'], effect: 'commit', execute: (_, values) => { commits++; return values.read; } },
  }, 'commit', { concurrency: 1, deadlineMs: 120 });
  const events = [];
  await assert.rejects(graph('late', undefined, { onEvent: event => events.push(event) }), { name: 'TimeoutError' });
  assert(events.some(event => event.fields.nodeId === 'commit' && event.fields.state === 'skipped'));
  const next = graph('next');
  await wait(25);
  assert.deepEqual(workers, ['late']);
  assert.equal(commits, 0);
  late('late');
  assert.equal(await next, 'next');
  assert.equal(commits, 1);
});

test('cancellation propagates to the worker and a failed dependency cannot dispatch a write', async () => {
  const controller = new AbortController();
  let started, stopped;
  const pending = new Promise(resolve => { started = resolve; });
  const run = defineDag({
    active: { needs: [], execute: (_, __, signal) => {
      started();
      return wait(200, undefined, { signal }).catch(error => { stopped = true; throw error; });
    } },
    write: { needs: ['active'], effect: 'commit', execute: () => assert.fail('cancelled write') },
  }, 'write', { deadlineMs: 1000 });
  const result = run(null, controller.signal, { onEvent: () => { throw new Error('collector offline'); } });
  await pending;
  controller.abort();
  await assert.rejects(result, { name: 'AbortError' });
  await wait(0);
  assert(stopped);
});

test('embodied agent coordination binds the same frame and refuses stale, untrusted or blocked results', async () => {
  const started = 5000;
  const missionInput = { workspaceId: 'workcell-1', missionId: 'mission-1' };
  for (const mode of ['clear', 'stale', 'wrong-scope', 'occupied', 'wrong-frame', 'forged-action']) {
    const run = createAgentCoordination({
      observe: scope => ({ ...scope, workspaceId: mode === 'wrong-scope' ? 'workcell-2' : scope.workspaceId, frameId: 'frame-1',
        state: mode === 'occupied' ? 'occupied' : 'clear', observedAt: mode === 'stale' ? 1000 : started }),
      mission: scope => ({ ...scope, intent: 'record_observation' }),
      plan: (scope, { frame, mission }) => ({ ...scope, frameId: frame.frameId,
        action: mode === 'forged-action' ? 'unknown_action' : mission.intent }),
      review: (scope, { frame, plan }) => ({ ...scope, frameId: mode === 'wrong-frame' ? 'frame-2' : frame.frameId,
        action: plan.action, approved: true }),
    }, { clock: () => started });
    const result = await run(missionInput);
    assert.equal(result.state, mode === 'clear' ? 'proposal' : 'hold');
    if (mode === 'clear') assert.deepEqual([result.approvalRequired, result.action, result.frameId],
      [true, 'record_observation', 'frame-1']);
  }
});
