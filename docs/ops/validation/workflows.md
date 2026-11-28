# 多工作流与扩展约定

CI 是持续集成：把源码检查、编译、隔离验证等流程自动编排。它与业务运行时的 DAG（有向无环任务图）不是同一层，不能把全部流程硬编码在一个文件或单一执行器分支里。

## 已有工作流

| 文件 | 责任 | 触发边界 |
| --- | --- | --- |
| [ci.yml](../../../.github/workflows/ci.yml) | 总入口、依赖顺序、手动选择 | 保持原 push、PR 和手动入口 |
| [verify-source.yml](../../../.github/workflows/verify-source.yml) | 双平台源码、前端和 Observer 契约 | 由总入口调用 |
| [verify-cpp-systems.yml](../../../.github/workflows/verify-cpp-systems.yml) | C++ 系统测试与 Sanitizer 矩阵 | 由总入口调用 |
| [verify-fleet-grpc.yml](../../../.github/workflows/verify-fleet-grpc.yml) | 预装 SDK 的车队隔离测试 | 仅显式手动运行，保留 isolated-ci 环境 |
| [verify-business.yml](../../../.github/workflows/verify-business.yml) | PostgreSQL、业务、OTLP 及运行包验证 | 源码门禁通过后，显式手动选择 |
| [verify-native.yml](../../../.github/workflows/verify-native.yml) | 已预装原生依赖与 Worker 验证 | 业务验证通过后，显式手动选择 |

这些流程只验证和生成制品，不自动替换正式服务。拆分保留原有检查命令、矩阵、超时、Action 固定提交和制品名，不把本地契约通过描述为 GitHub 远端运行成功。

## 后续新增接口

1. 在 `.github/workflows/` 增加独立、单一职责的 YAML 文件，以 `on.workflow_call` 声明可复用入口，不复制整个总入口。
2. 在 [workflow-catalog.json](../../../.github/workflow-catalog.json) 登记 `id`、`path`、`kind`、`jobs`、`manualOnly`、`effects`、`inputs` 和 `requires`。注册表允许增加条目，不写死工作流总数。
3. 在总入口新增一个 `jobs.<id>.uses: ./.github/workflows/<file>.yml`，用 `needs` 声明依赖。需要输入时在调用方与被调用方声明一致的 boolean、number 或 string 类型。
4. 不暴露任意 shell 命令、任意 Runner 或任意部署地址作为输入。权限默认 `contents: read`；不使用 `secrets: inherit` 隐式传递所有凭据。
5. 会访问预配置隔离 Runner 的流程，在调用方和被调用作业内同时保留手动事件门禁，保留环境审批。未来生产发布工作流必须另行设计和批准，不能混入普通 PR。
6. 运行 `python scripts/project/check_workflows.py` 与 `python tests/project/test_workflow_catalog.py`，再跑源码 CI。检查覆盖目录注册、调用目标、依赖循环、权限、手动门禁和固定 Action 提交，但不替代 GitHub 的 YAML 与平台运行校验。

可复用工作流语法参照 [GitHub 官方说明](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows)。本次仅访问公开文档，没有向外发送源码。

## 业务工作流边界

`backend/workflow/` 是通用执行引擎，不是“只支持一种业务”的目录。现有 Agent 提案和审批分别构造 `WorkflowGraph` 并执行；未来的新业务图应留在自己的业务领域，复用引擎，而不是加入一个全局大 switch。具体接口与生命周期见 [业务工作流说明](../../../backend/workflow/README.md)。

[复现与交付](reproduce.md) · [C++ 系统验证](cpp-systems.md)