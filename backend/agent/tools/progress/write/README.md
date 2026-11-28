# `progress.write`

Owner-scoped progress.write port. No raw database, shell or transaction interface.

- Contract: `spec.py`; version: 1; effect: `external`.
- Preconditions: authorized actor, schema validation and source ownership checks.
- Idempotency field: `idempotencyKey`
- Bounds: Timeout: 2000 ms. Output: 8192 bytes; items: None; freshness budget: 30 s.
- Parameters: see `SPEC.parameters` in `spec.py`; unknown fields are rejected.
- Failure envelope: `invalid`, `denied`, `not_found`, `timeout`, `unavailable` or `conflict`; never raw internal errors.
- Call through the authenticated ToolRuntime / SkillRuntime, not the implementation directly.
- Source implementations still reside in `tool_runtime/workspace.py` and `tool_runtime/nebula.py`; per-port implementation extraction is a later migration step.

## 参数示例（仅示例，不会执行）

```json
{
  "date": "2026-11-27",
  "subject": "example",
  "itemId": "example",
  "minutes": 0,
  "idempotencyKey": "example"
}
```

真实写入必须由可信 UI 明确确认，并使用同一幂等键重试；模型不能伪造确认凭证。
