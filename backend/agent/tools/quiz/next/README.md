# `quiz.next`

Owner-scoped quiz.next port. No raw database, shell or transaction interface.

- Contract: `spec.py`; version: 1; effect: `read`.
- Preconditions: authorized actor, schema validation and source ownership checks.
- Idempotency field: `None: read-only or staged proposal; no automatic external action.`
- Bounds: Timeout: 1000 ms. Output: 8192 bytes; items: None; freshness budget: 30 s.
- Parameters: see `SPEC.parameters` in `spec.py`; unknown fields are rejected.
- Failure envelope: `invalid`, `denied`, `not_found`, `timeout`, `unavailable` or `conflict`; never raw internal errors.
- Call through the authenticated ToolRuntime / SkillRuntime, not the implementation directly.
- Source implementations still reside in `tool_runtime/workspace.py` and `tool_runtime/nebula.py`; per-port implementation extraction is a later migration step.

## 参数示例（仅示例，不会执行）

```json
{
  "subject": "example",
  "excludeMastered": true
}
```
