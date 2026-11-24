const identifier = /^[A-Za-z0-9_.-]{1,80}$/;
const tracePattern = /^(?!0+$)[a-f0-9]{32}$/;
export const workflowEventStates = Object.freeze({
  run: Object.freeze(['started', 'success', 'failed', 'timeout', 'cancelled', 'uncertain']),
  node: Object.freeze(['queued', 'started', 'success', 'failed', 'timeout', 'retry', 'skipped', 'cancelled', 'uncertain'])
});
const token = length => Array.from(crypto.getRandomValues(new Uint8Array(length)), byte => byte.toString(16).padStart(2, '0')).join('');

function bounded(value, min, max, label) {
  if (!Number.isSafeInteger(value) || value < min || value > max) throw new Error('无效的 DAG ' + label);
  return value;
}

function timeout(label) {
  return new DOMException(label, 'TimeoutError');
}

function waitWithSignal(promise, signal) {
  return new Promise((resolve, reject) => {
    const stop = () => { signal.removeEventListener('abort', stop); reject(signal.reason); };
    signal.addEventListener('abort', stop, { once: true });
    if (signal.aborted) stop();
    promise.then(resolve, reject).finally(() => signal.removeEventListener('abort', stop));
  });
}

function delay(ms, signal) {
  if (!ms) return Promise.resolve();
  let timer;
  return waitWithSignal(new Promise(resolve => { timer = setTimeout(resolve, ms); }), signal).finally(() => clearTimeout(timer));
}

class Workers {
  constructor(capacity) {
    this.available = capacity;
    this.pending = [];
  }

  acquire(signal) {
    signal.throwIfAborted();
    if (this.available) {
      this.available--;
      return Promise.resolve(() => this.release());
    }
    return new Promise((resolve, reject) => {
      const run = () => {
        signal.removeEventListener('abort', cancel);
        resolve(() => this.release());
      };
      const cancel = () => {
        this.pending.splice(this.pending.indexOf(run), 1);
        reject(signal.reason);
      };
      this.pending.push(run);
      signal.addEventListener('abort', cancel, { once: true });
    });
  }

  release() {
    const next = this.pending.shift();
    if (next) next();
    else this.available++;
  }
}

function outcome(error, signal) {
  if (error?.code === 'outcome_uncertain') return 'uncertain';
  const cause = signal?.aborted ? signal.reason : error;
  return cause?.name === 'TimeoutError' ? 'timeout' : cause?.name === 'AbortError' || signal?.aborted ? 'cancelled' : 'failed';
}

function events(workflow, traceId, runId, send) {
  let sequence = 0;
  let latest = 0n;
  return (eventType, fields, spanId) => {
    if (!send) return;
    const allowed = workflowEventStates[eventType.slice(4)];
    if (allowed && !allowed.includes(fields.state)) throw new Error('Invalid workflow event state');
    const wall = BigInt(Date.now()) * 1000000n;
    latest = wall > latest ? wall : latest + 1n;
    const event = { schemaVersion: 1, eventId: runId + '.' + (++sequence), eventType,
      occurredAt: String(latest), traceId, fields: { runId, ...fields } };
    if (eventType === 'dag.run') event.fields.workflow = workflow;
    if (spanId) event.spanId = spanId;
    try { Promise.resolve(send(event)).catch(() => {}); } catch { /* A collector may be offline. */ }
  };
}

/** Compile a trusted graph once. Model output belongs in node inputs, never in node definitions. */
export function defineDag(nodes, target, { workflow = 'workflow.v1', concurrency = 4, maxRuns = 4, deadlineMs = 195000 } = {}) {
  if (typeof workflow !== 'string' || !identifier.test(workflow) || Object.keys(nodes).length > 64) throw new Error('无效的 DAG 工作流');
  const workers = new Workers(bounded(concurrency, 1, 16, '并发'));
  bounded(maxRuns, 1, 8, '执行容量');
  bounded(deadlineMs, 1, 600000, '总时限');
  const order = new Map(), visiting = new Set(), reachable = new Set();
  function validate(name, included) {
    if (visiting.has(name)) throw new Error('DAG 中存在环：' + name);
    if (order.has(name)) return;
    if (!Object.hasOwn(nodes, name)) throw new Error('DAG 缺少节点：' + name);
    if (!identifier.test(name)) throw new Error('无效的 DAG 节点编号');
    const { needs, execute, effect = 'compute', timeoutMs = deadlineMs, attempts = 1, backoffMs = 30, acceptIncomplete = false } = nodes[name];
    if (!Array.isArray(needs) || new Set(needs).size !== needs.length) throw new Error('DAG 依赖须为不同节点');
    visiting.add(name);
    for (const dependency of needs) validate(dependency, included);
    visiting.delete(name);
    if (typeof execute !== 'function' || !['read', 'compute', 'commit'].includes(effect) ||
        (effect === 'commit' && (attempts !== 1 || acceptIncomplete))) throw new Error('无效的 DAG 节点执行或重试策略：' + name);
    bounded(timeoutMs, 1, deadlineMs, '节点时限');
    bounded(attempts, 1, 3, '尝试次数');
    bounded(backoffMs, 0, 5000, '重试间隔');
    if (included) reachable.add(name);
    order.set(name, { needs: [...needs], execute, effect, timeoutMs, attempts, backoffMs, acceptIncomplete });
  }
  validate(target, true);
  for (const name of Object.keys(nodes)) validate(name, false);

  let runs = 0;
  return async (input, callerSignal, { onEvent, traceId = token(16) } = {}) => {
    if (typeof traceId !== 'string' || !tracePattern.test(traceId)) throw new Error('无效的 DAG traceId');
    callerSignal?.throwIfAborted();
    if (runs >= maxRuns) throw Object.assign(new Error('DAG 执行容量已满'), { code: 'dag_busy' });
    const deadline = new AbortController();
    const signal = callerSignal ? AbortSignal.any([callerSignal, deadline.signal]) : deadline.signal;
    const timer = setTimeout(() => deadline.abort(timeout('工作流超时')), deadlineMs);
    const runId = token(12), emit = events(workflow, traceId, runId, onEvent);
    const running = new Map();
    runs++;
    emit('dag.run', { state: 'started' });
    for (const name of reachable) for (const predecessor of order.get(name).needs) emit('dag.edge', { predecessor, successor: name });

    async function invoke(name, node, dependencies) {
      const blocked = dependencies.find(dependency => dependency.state !== 'success');
      if (signal.aborted || (blocked && !node.acceptIncomplete)) {
        const error = signal.aborted ? signal.reason : blocked.error;
        const state = signal.aborted ? outcome(error, signal) : 'skipped';
        emit('dag.node', { nodeId: name, attempt: 1, state }, token(8));
        return { state, error };
      }
      const values = Object.fromEntries(node.needs.map((key, i) => [key, dependencies[i].value]));
      const upstream = Object.freeze(Object.fromEntries(node.needs.map((key, i) => [key, dependencies[i].state])));
      for (let attempt = 1; attempt <= node.attempts; attempt++) {
        const spanId = token(8), identity = { nodeId: name, attempt };
        emit('dag.node', { ...identity, state: 'queued' }, spanId);
        let started, alarm, attemptSignal, invoked = false;
        try {
          const release = await workers.acquire(signal);
          if (signal.aborted) { release(); signal.throwIfAborted(); }
          started = performance.now();
          const deadline = new AbortController();
          attemptSignal = AbortSignal.any([signal, deadline.signal]);
          alarm = setTimeout(() => deadline.abort(timeout('节点超时：' + name)), node.timeoutMs);
          emit('dag.node', { ...identity, state: 'started', inputCount: node.needs.length }, spanId);
          const task = Promise.resolve().then(() => {
            attemptSignal.throwIfAborted();
            invoked = true;
            return node.execute(input, values, attemptSignal,
              { runId, traceId, spanId, attempt, idempotencyKey: runId + '.' + name, upstream });
          });
          // A worker owns its slot until the actual operation exits, including late completions.
          task.then(release, release);
          const value = await waitWithSignal(task, attemptSignal);
          attemptSignal.throwIfAborted();
          emit('dag.node', { ...identity, state: 'success', durationMs: performance.now() - started,
            outputCount: Number(value !== undefined) }, spanId);
          return { state: 'success', value };
        } catch (error) {
          let state = outcome(error, attemptSignal ?? signal);
          if (node.effect === 'commit' && invoked && (state === 'timeout' || state === 'cancelled')) {
            state = 'uncertain';
            error = Object.assign(new Error('写入结果待确认', { cause: error }), { code: 'outcome_uncertain' });
          }
          const retryable = state === 'timeout' || (state === 'failed' && error?.retryable === true);
          const fields = { ...identity, state, errorCode: state === 'failed' ? (retryable ? 'transient' : 'execution_failed') : state };
          if (started !== undefined) fields.durationMs = performance.now() - started;
          emit('dag.node', fields, spanId);
          if (signal.aborted || !retryable || attempt === node.attempts) return { state, error };
          emit('dag.node', { ...identity, state: 'retry' }, spanId);
        } finally { clearTimeout(alarm); }
        try { await delay(node.backoffMs * attempt, signal); }
        catch (error) { return { state: outcome(error, signal), error }; }
      }
    }

    function run(name) {
      if (!running.has(name)) {
        const node = order.get(name);
        running.set(name, Promise.all(node.needs.map(run)).then(dependencies => invoke(name, node, dependencies)));
      }
      return running.get(name);
    }

    try {
      const result = await run(target);
      if (result.state === 'uncertain' || result.error?.code === 'outcome_uncertain') throw result.error;
      signal.throwIfAborted();
      if (result.state !== 'success') throw result.error ?? new Error('DAG 节点未完成');
      emit('dag.run', { state: 'success' });
      return result.value;
    } catch (error) {
      emit('dag.run', { state: outcome(error, signal) });
      throw error;
    } finally {
      clearTimeout(timer);
      runs--;
    }
  };
}
