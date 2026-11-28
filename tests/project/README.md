# 项目级验证导航

本目录按“被测职责”导航，保留现有直接执行路径、辅助模块导入和 CI 注册。文件多不等于职责混乱，不通过搬文件制造新的导入兼容层。

## 推荐入口

```bash
# 已有 Node/Python 环境；不安装依赖、不配置真实业务库
python scripts/project/ci_checks.py source --observer-http

# 单项示例：目录与无副作用模拟测试
python tests/project/test_script_utilities_layout.py
python tests/project/test_workflow_catalog.py
```

`source` 会使用本机回环 HTTP、临时目录与子进程；并非所有文件都是纯静态检查。Windows 若没有 Bash/Lua，相应可选运行测试会显式跳过，不能当作已通过。`full` 档需要预先配置的 Linux 构建和明确隔离的 PostgreSQL，不在日常导航中自动执行。

| 职责 | 文件 |
| --- | --- |
| Agent、技能与权限 | [目录](test_agent_catalog_layout.py)、[控制集成](test_agent_control_integration.py)、[路由](test_agent_control_routes.py)、[工具适配](test_nebula_tool_adapters.py)、[技能运行](test_skill_runtime.py)、[工具运行](test_tool_runtime.py)、[工作台 HTTP](test_workbench_http.py)、[控制台前端](test_agent_control_frontend.cjs)、[人员权限前端](test_personnel_frontend.cjs)、[启动配置](test_control_startup_config.py) |
| CI、API 与工作流 | [API](test_api_contracts.py)、[发布门禁](test_ci_gates.py)、[C++ 系统门禁](test_cpp_systems_gate.py)、[可复用工作流](test_workflow_catalog.py)、[源码清单](test_source_inventory.py) |
| 目录与迁移证据 | [AI C++](test_ai_cpp_layout.py)、[AI 脚本](test_ai_script_layout.py)、[应用领域](test_application_layout.py)、[应用观测](test_application_observability_layout.py)、[监控工具](test_observability_tools_layout.py)、[Observer](test_observer_layout.py)、[业务运维](test_phase11_operations_layout.py)、[通用脚本](test_script_utilities_layout.py)、[换行证据契约](test_layout_evidence.py) |
| 模型与 Worker | [AI 流水线](test_ai_pipeline_contracts.py)、[每日 Worker](test_daily_onnx_worker.py)、[模型选择](test_model_selection.py)、[模型比较](test_model_benchmark.py)、[gRPC TLS](test_grpc_tls_compatibility.py) |
| PostgreSQL 与测量 | [A/A、A/B 门禁](test_postgres_tuning_gate.py)、[安全设置](test_postgres_settings.py)、[活动诊断](test_postgres_activity.py)、[测量证据](test_measurement_evidence.py) |
| 前端与网关 | [导入图](test_frontend_graph.py)、[版本化资源](test_frontend_release_assets.py)、[网关](test_gateway.py)、[TLS 前置检查](test_gateway_tls_preflight.py) |
| 运行与可观测性 | [分布式观测](test_distributed_observability.py)、[HTTP 观测](test_observability.py)、[查询](test_observer_queries.py)、[进程身份](test_process_identity.py)、[运行交付](test_runtime_delivery.py)、[管理员 CLI](test_site_admin_cli.py)、[快照裁剪](test_snapshot_pruning.py) |
| 浏览器验收脚本 | [总览](observer_browser.cjs)、[仪表盘](observer_dashboard_browser.cjs)、[日志](observer_logging_browser.cjs)、[资源](observer_resources_browser.cjs)、[工作区](observer_workspace_browser.cjs)；需已有浏览器环境，不能与静态语法检查混称为真实浏览器验收 |

`layout_evidence.py` 是迁移测试专用辅助模块，只接受 Git 的统一 LF/CRLF 文本表示变化，不用于模型、依赖或运行时安全校验。新增测试应在 [ci_checks.py](../../scripts/project/ci_checks.py) 中注册到正确验证阶段；平台扩展还应同步 [工作流目录](../../.github/workflow-catalog.json) 和对应契约。C++ 多 DAG 定义的运行契约位于 [WorkflowContract.cpp](../cpp_systems/WorkflowContract/WorkflowContract.cpp)，不是这里的 GitHub Actions 测试。

[项目目录边界](../../docs/architecture/overview/project-layout.md) · [多工作流说明](../../docs/ops/validation/workflows.md)
