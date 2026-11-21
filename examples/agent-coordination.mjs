import { createAgentCoordination } from '../frontend/phase11/shared/AgentRuntime/AgentCoordination.js';

const mode = process.argv[2] ?? 'retry';
if (!['retry', 'stale', 'occupied', 'unavailable'].includes(mode)) {
  console.error('用法：node examples/agent-coordination.mjs [retry|stale|occupied|unavailable]');
  process.exit(2);
}
let attempts = 0;
const run = createAgentCoordination({
  async observe(scope) {
    if (++attempts === 1 && mode === 'retry') throw Object.assign(new Error('测试观测暂不可用'), { retryable: true });
    if (mode === 'unavailable') throw new Error('测试设备离线');
    return { ...scope, frameId: 'demo-frame-017', state: mode === 'occupied' ? 'occupied' : 'clear',
      observedAt: Date.now() - (mode === 'stale' ? 3000 : 0) };
  },
  mission: scope => ({ ...scope, intent: 'record_observation' }),
  plan: (scope, { frame, mission }) => ({ ...scope, frameId: frame.frameId, action: mission.intent }),
  review: (scope, { frame, plan }) => ({ ...scope, frameId: frame.frameId, action: plan.action, approved: true }),
});
const events = [];
const result = await run({ workspaceId: 'simulation', missionId: 'demo-mission' }, undefined, { onEvent: event => events.push(event) });
console.log(JSON.stringify({ simulated: true, mode, result, events }));
