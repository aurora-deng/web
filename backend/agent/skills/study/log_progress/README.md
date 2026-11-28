# `study.log_progress`

Commit one confirmed progress record and its replay receipt atomically.

- Contract: `spec.py`; version: 1; effect: `external`.
- Preconditions: authorized_actor, confirmed_user_intent, atomic_idempotent_port.
- Idempotency field: `idempotencyKey`
- Bounds: Timeout: 2500 ms.
- Parameters: see `SPEC.parameters` in `spec.py`; unknown fields are rejected.
- Failure envelope: `invalid`, `denied`, `not_found`, `timeout`, `unavailable` or `conflict`; never raw internal errors.
- Call through the authenticated ToolRuntime / SkillRuntime, not the implementation directly.
- Implementation: `impl.py`; confirmation, audit and deadline enforcement remain in the runtime.

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
