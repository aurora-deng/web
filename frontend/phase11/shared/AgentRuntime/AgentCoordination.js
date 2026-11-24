import { defineDag } from './DagExecutor.js?v=1b40f8c19ed29802';

const id = /^[A-Za-z0-9_.-]{1,80}$/;
const actions = new Set(['record_observation', 'summarize_observation']);
const states = new Set(['clear', 'occupied', 'unknown']);
const boundedId = value => typeof value === 'string' && id.test(value);
const fresh = (at, now, maxAge) => Number.isSafeInteger(at) && at <= now && now - at <= maxAge;

function matchingScope(value, scope) {
  return value && value.workspaceId === scope.workspaceId && value.runId === scope.runId;
}

/** Exercise the coordination boundary using metadata and a proposal. A device adapter belongs outside this graph. */
export function createAgentCoordination(adapters, { clock = Date.now, maxAgeMs = 2000 } = {}) {
  if (!Number.isSafeInteger(maxAgeMs) || maxAgeMs < 100 || maxAgeMs > 10000) throw new Error('无效的观测时效');
  for (const port of ['observe', 'mission', 'plan', 'review']) if (typeof adapters[port] !== 'function') {
    throw new Error('缺少协作能力：' + port);
  }
  const readScope = (input, context) => Object.freeze({
    workspaceId: input.workspaceId, missionId: input.missionId, runId: context.runId, traceId: context.traceId,
  });
  const validFrame = (frame, scope) => matchingScope(frame, scope) && boundedId(frame.frameId) && states.has(frame.state) &&
    fresh(frame.observedAt, clock(), maxAgeMs);
  const hold = reason => Object.freeze({ state: 'hold', reason });
  const run = defineDag({
    observe: { needs: [], effect: 'read', attempts: 2, timeoutMs: 500, async execute(input, _, signal, context) {
      const scope = readScope(input, context);
      const frame = await adapters.observe(scope, { signal, idempotencyKey: context.idempotencyKey });
      if (!validFrame(frame, scope)) throw new Error('观测帧已过期或来源不匹配');
      return Object.freeze({ frameId: frame.frameId, state: frame.state, observedAt: frame.observedAt });
    } },
    mission: { needs: [], effect: 'read', timeoutMs: 500, async execute(input, _, signal, context) {
      const scope = readScope(input, context);
      const task = await adapters.mission(scope, { signal, idempotencyKey: context.idempotencyKey });
      if (!matchingScope(task, scope) || task.missionId !== scope.missionId || !actions.has(task.intent)) {
        throw new Error('任务来源或类别无效');
      }
      return Object.freeze({ id: task.missionId, intent: task.intent });
    } },
    plan: { needs: ['observe', 'mission'], timeoutMs: 700, async execute(input, { observe, mission }, signal, context) {
      const scope = readScope(input, context);
      const plan = await adapters.plan(scope, { frame: observe, mission }, { signal, idempotencyKey: context.idempotencyKey });
      if (!matchingScope(plan, scope) || plan.frameId !== observe.frameId || plan.action !== mission.intent) {
        throw new Error('计划目标不符合已登记任务');
      }
      return Object.freeze({ frameId: plan.frameId, action: plan.action });
    } },
    review: { needs: ['observe', 'plan'], timeoutMs: 500, async execute(input, { observe, plan }, signal, context) {
      const scope = readScope(input, context);
      const verdict = await adapters.review(scope, { frame: observe, plan }, { signal, idempotencyKey: context.idempotencyKey });
      return matchingScope(verdict, scope) && verdict.frameId === observe.frameId && verdict.action === plan.action &&
        verdict.approved === true;
    } },
    decide: { needs: ['observe', 'mission', 'plan', 'review'], acceptIncomplete: true,
      execute(input, { observe, mission, plan, review }, _, context) {
        if (Object.values(context.upstream).some(state => state !== 'success')) return hold('dependency_unavailable');
        if (!fresh(observe.observedAt, clock(), maxAgeMs)) return hold('stale_observation');
        if (observe.state !== 'clear' || !review) return hold('safety_blocked');
        return Object.freeze({ state: 'proposal', approvalRequired: true, workspaceId: input.workspaceId,
          missionId: mission.id, frameId: observe.frameId, action: plan.action, traceId: context.traceId });
      } },
  }, 'decide', { workflow: 'embodied.coordination.v1', concurrency: 3, maxRuns: 2, deadlineMs: 3000 });
  return (input, signal, options) => {
    if (!boundedId(input?.workspaceId) || !boundedId(input?.missionId)) throw new Error('缺少有效的协作空间或任务');
    return run(Object.freeze({ workspaceId: input.workspaceId, missionId: input.missionId }), signal, options);
  };
}
