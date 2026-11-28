# Agent 目录与事务边界

- `tools/<域>/<名称>/spec.py + impl.py + README.md`：18 个工具的声明、业务实现及说明。
- `skills/<域>/<名称>/spec.py + impl.py + README.md`：19 个技能的声明、确定性配方及说明。
- 多级名字按段建目录，如 `fleet.robot.quarantine` → `skills/fleet/robot/quarantine/`。
- `catalog.json` 保留显式清单和稳定顺序；加载器拒绝未知适配器、缺失实现、额外声明和目录越界。
- 启动时校验并按进程缓存契约、实现和配方；返回独立的映射副本，请求期间不反复扫描磁盘。变更后需重启进程。

## 已迁移的实现

每个工具的 `impl.py` 按数据源提供 `read_workspace` / `read_nebula`，或 `write_workspace` / `write_nebula`。
15 个读工具和 3 个写工具均通过固定清单分派；未增加任何新的工具权限。
`tool_runtime/workspace.py` 与 `nebula.py` 保留鉴权、截止时间、锁、事务、幂等回执和补偿边界。

**业务写入和回执必须在同一事务中提交。** 工具实现不能独自提交事务或暴露 SQL、数据库连接、Shell、任意文件和 URL 访问。
未知工具仍为 `not_found`；跨账号仍为 `denied`；过期请求仍为 `timeout`。
`workflow.get_run` 的检查点投影保持 `fresh=False`，不得充当实时状态。

`tests/project/fixtures/agent_catalog_v1.json` 保留迁移前契约快照。
C++ `SkillCatalog.generated.inc` 保持原有字节和生成结果。

## 离线验证

```text
python tests/project/test_agent_catalog_layout.py
python tests/project/test_tool_runtime.py
python tests/project/test_skill_runtime.py
python tests/project/test_nebula_tool_adapters.py
python scripts/project/generate_skill_catalog.py --check
```

Nebula 测试使用真实的 `client/yantu/runtime/server.py::Storage` 和合成账号 SQLite：覆盖全部 15 个读入口、3 个写入口、同键并发重放、回执失败回滚、适配器重建和不覆盖用户修改的撤销。
原生聊天及 Fleet 使用固定测试端口的模拟对象，不向网络发请求，也不代表线上接口、gRPC Worker 或登录页面已经验收。

## 仍未完成

`backend/ai` 的 C++ 目录移动、CMake 引用调整、独立 AI 准备执行器和 gRPC Worker 日常服务接入仍需实现与 Linux 编译验证。
本轮实现尚未部署，不能以离线通过替代生产验收。
