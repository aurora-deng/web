import { composeContext, compressHistory, planContext } from './ContextWindow.js?v=4364119c78f60e44';
import { limitedAnalysis } from './PersonalAgent.js?v=4364119c78f60e44';
import { defineDag } from '../AgentRuntime/DagExecutor.js?v=4364119c78f60e44';

export { defineDag };

async function generate(input, plan, signal) {
  const result = await input.generate(plan, signal);
  if (result?.state === 'completed') return result;
  if (result?.state === 'cancelled') throw new DOMException('已停止回复', 'AbortError');
  throw new Error(result?.error || '生成未完成');
}

export const runAgentTurn = defineDag({
  profile: { needs: [], effect: 'read', execute: ({ agent }) => agent?.profile().catch(() => null) },
  window: { needs: [], effect: 'read', async execute(input, _, signal) {
    const [history, checkpoint] = await Promise.all([input.readHistory(signal), input.readCheckpoint(signal)]);
    return planContext(history, checkpoint, input.text, input.mode);
  } },
  compact: { needs: ['profile', 'window'], execute(input, { profile, window }, signal) {
    if (window.older.length) input.onStage?.('compacting');
    const learn = Boolean(profile?.enabled);
    return compressHistory(window, input.text, {
      learn, useModel: learn && profile.useModel && Boolean(input.infer), signal,
      infer: (messages, parent) => limitedAnalysis(input.infer, messages, parent, 320, 4200),
    });
  } },
  preferences: { needs: ['profile', 'compact'], execute(input, { profile, compact }, signal) {
    return input.agent?.prepare(input.text, input.id, {
      profileSnapshot: profile, infer: compact.changed ? null : input.infer,
      recovered: compact.learned, signal,
    });
  } },
  compose: { needs: ['window', 'compact', 'preferences'], execute(input, state) {
    const { checkpoint } = state.compact;
    const guidance = state.preferences?.guidance || '';
    return {
      guidance, checkpoint, maxTokens: state.preferences?.maxTokens ?? 512, recent: state.window.recent, changed: state.compact.changed,
      turn: state.preferences?.turn,
      messages: composeContext(input.text, state.window.recent, guidance, checkpoint),
    };
  } },
  answer: { needs: ['compose'], async execute(input, { compose }, signal) {
    input.onStage?.('answering');
    try {
      return { result: await generate(input, compose, signal), plan: compose };
    } catch (error) {
      if (input.mode !== 'web' || error.code !== 'context_length_exceeded') throw error;
      signal.throwIfAborted();
      const smaller = planContext(compose.recent, compose.checkpoint, input.text, 'web', true);
      if (!smaller.older.length) throw error;
      input.onStage?.('compacting');
      const { checkpoint, learned } = await compressHistory(smaller, input.text, {
        learn: Boolean(compose.turn), useModel: false, signal,
      });
      const turn = compose.turn ? { ...compose.turn, recovered: {
        historical: { ...compose.turn.recovered?.historical, ...learned.historical },
        recent: [...(compose.turn.recovered?.recent ?? []), ...learned.recent].slice(-3),
      } } : null;
      const plan = { ...compose, checkpoint, turn, changed: true,
        messages: composeContext(input.text, smaller.recent, compose.guidance, checkpoint) };
      input.onStage?.('answering');
      return { result: await generate(input, plan, signal), plan };
    }
  } },
  checkpoint: { needs: ['answer'], effect: 'commit', async execute(input, { answer }, signal) {
    if (answer.plan.changed && answer.result?.state === 'completed') {
      try { await input.saveCheckpoint(answer.plan.checkpoint, signal); }
      catch { input.onStage?.('checkpoint-failed'); }
    }
    return answer;
  } },
  remember: { needs: ['checkpoint'], effect: 'commit', async execute(input, { checkpoint: { result, plan } }) {
    if (result?.state === 'completed' && input.agent) {
      const saved = await input.agent.complete(plan.turn).catch(() => null);
      input.agent.summarize(saved, input.infer).catch(() => {});
    }
    return result ? { ...result, compacted: plan.changed } : result;
  } },
}, 'remember', { workflow: 'xiaoyan.turn.v1', concurrency: 4, maxRuns: 4 });
