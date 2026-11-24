const finished = new Set(["success", "failed", "timeout", "uncertain", "skipped", "cancelled"]);

// Longest dependency chain by execution time. Retry attempts count once each.
// Queue wait, network transport and gaps between attempts are outside this metric.
export function criticalPaths(events) {
  const runs = new Map();
  for (const event of events) {
    if (!["dag.run", "dag.node", "dag.edge"].includes(event.eventType)) continue;
    const id = event.fields.runId;
    if (!runs.has(id)) runs.set(id, []);
    runs.get(id).push(event);
  }
  return [...runs].map(([runId, items]) => {
    const nodes = new Map(), attempts = new Map();
    const node = id => {
      if (!nodes.has(id)) nodes.set(id, { parents: new Set(), ms: 0, measured: false });
      return nodes.get(id);
    };
    const terminalRun = items.some(event => event.eventType === "dag.run" && finished.has(event.fields.state));
    for (const event of items) {
      const fields = event.fields;
      if (event.eventType === "dag.edge") {
        node(fields.predecessor);
        node(fields.successor).parents.add(fields.predecessor);
      }
      if (event.eventType !== "dag.node") continue;
      node(fields.nodeId);
      const key = fields.nodeId + ":" + fields.attempt;
      if (!attempts.has(key)) attempts.set(key, { id: fields.nodeId });
      const attempt = attempts.get(key);
      if (fields.state === "started") attempt.start = event.occurredAt;
      if (finished.has(fields.state)) attempt.end = event;
    }
    let partial = !terminalRun;
    for (const attempt of attempts.values()) {
      const end = attempt.end;
      if (!end) {
        partial = true;
        continue;
      }
      const duration = end.fields.durationMs;
      const current = node(attempt.id);
      if (attempt.start === undefined && end.fields.state !== "skipped") {
        partial = true;
        continue;
      }
      const calculated = attempt.start === undefined ? 0 : Number(BigInt(end.occurredAt) - BigInt(attempt.start)) / 1e6;
      current.ms += Number.isFinite(duration) && duration >= 0 ? duration : Math.max(0, calculated);
      current.measured = true;
    }
    const visiting = new Set(), complete = new Map();
    let cyclic = false;
    function walk(id) {
      if (complete.has(id)) return complete.get(id);
      const current = nodes.get(id);
      if (visiting.has(id)) {
        cyclic = true;
        return { nodes: [], ms: 0 };
      }
      visiting.add(id);
      let best = { nodes: [], ms: 0 };
      for (const parent of [...current.parents].sort()) {
        const chain = walk(parent);
        if (chain.ms > best.ms || (chain.ms === best.ms && chain.nodes.length > best.nodes.length)) best = chain;
      }
      visiting.delete(id);
      const chain = { nodes: [...best.nodes, id], ms: best.ms + current.ms };
      complete.set(id, chain);
      return chain;
    }
    let longest = { nodes: [], ms: 0 };
    for (const id of [...nodes.keys()].sort()) {
      if (!nodes.get(id).measured) partial = true;
      const chain = walk(id);
      if (chain.ms > longest.ms || (chain.ms === longest.ms && chain.nodes.length > longest.nodes.length)) longest = chain;
    }
    return { runId, nodes: cyclic ? [] : longest.nodes, ms: cyclic ? 0 : longest.ms,
      partial: partial || cyclic || !nodes.size };
  });
}
