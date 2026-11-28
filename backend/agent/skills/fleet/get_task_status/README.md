# `fleet.get_task_status`

Read one task status without mutation.

- Contract: `spec.py`; version: 1; effect: `read`.
- Preconditions: authorized_actor.
- Idempotency field: `None: read-only or staged proposal; no automatic external action.`
- Bounds: Timeout: 1500 ms.
- Parameters: see `SPEC.parameters` in `spec.py`; unknown fields are rejected.
- Failure envelope: `invalid`, `denied`, `not_found`, `timeout`, `unavailable` or `conflict`; never raw internal errors.
- Call through the authenticated ToolRuntime / SkillRuntime, not the implementation directly.
- Implementation: `impl.py`; confirmation, audit and deadline enforcement remain in the runtime.

## 参数示例（仅示例，不会执行）

```json
{
  "taskId": "example"
}
```
