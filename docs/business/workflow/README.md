# 后端 C++ DAG 与可恢复工作流 · 业务说明

`backend/workflow/` 是独立 C++ 模块。WorkflowGraph 校验依赖并生成拓扑顺序，DagExecutor 管理有界并发、节点预算、超时、取消和重试。DurableWorkflow 可以使用后端的 SQLite 工作流日志恢复检查点。工作流保持稳定的操作键，外部动作结果不明确时进入待核实状态。

Fleet 的边缘任务与车队 Agent 在后端使用 DAG 原语。通用恢复模块有单独的持久化合同，车队工单、边缘结果及 Agent 审批由各自账本保证。生产接入新动作之前，需要明确副作用、超时后对账、版本兼容和关机恢复。

[DAG 执行器](../../../backend/workflow/DagExecutor/DagExecutor.cpp) · [检查点实现](../../../backend/workflow/DurableWorkflow/DurableWorkflow.cpp) · [SQLite 日志](../../../backend/systems/SqliteWorkflowJournal/SqliteWorkflowJournal.cpp) · [C++ 系统调用图](../../architecture/overview/cpp-systems.md)

[实操学习](learning.md) · [业务目录](../README.md)
