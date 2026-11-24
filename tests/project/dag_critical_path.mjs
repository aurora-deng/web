import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

const file = readFileSync(new URL("../../frontend/observability/dag_critical_path.js", import.meta.url));
const { criticalPaths } = await import("data:text/javascript;base64," + file.toString("base64"));

function row(eventType, fields, ms = 0) {
  return { eventType: "dag." + eventType, fields: { runId: "run", ...fields }, occurredAt: String(ms * 1000000) };
}

function attempt(nodeId, attemptNo, start, end, state = "success") {
  return [
    row("node", { nodeId, attempt: attemptNo, state: "started" }, start),
    row("node", { nodeId, attempt: attemptNo, state, durationMs: end - start }, end)
  ];
}

test("fork-join uses the longest dependency chain and includes both attempts", () => {
  const edges = [["A", "B"], ["A", "C"], ["B", "D"], ["C", "D"]];
  const input = [
    row("run", { state: "started" }),
    ...edges.map(([predecessor, successor]) => row("edge", { predecessor, successor })),
    ...attempt("A", 1, 0, 4, "timeout"),
    ...attempt("A", 2, 5, 7),
    ...attempt("B", 1, 7, 11),
    ...attempt("C", 1, 7, 29),
    ...attempt("D", 1, 29, 31),
    row("run", { state: "success" }, 31)
  ];
  assert.deepEqual(criticalPaths(input), [{ runId: "run", nodes: ["A", "C", "D"], ms: 30, partial: false }]);
});

test("partial and cyclic data are flagged without fabricating a complete path", () => {
  assert.equal(criticalPaths([row("node", { nodeId: "A", attempt: 1, state: "started" })])[0].partial, true);
  const loop = criticalPaths([
    row("edge", { predecessor: "A", successor: "B" }),
    row("edge", { predecessor: "B", successor: "A" }),
    row("run", { state: "success" })
  ]);
  assert.deepEqual(loop, [{ runId: "run", nodes: [], ms: 0, partial: true }]);
});
