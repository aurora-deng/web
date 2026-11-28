# 通用业务工作流引擎

此目录只放业务无关的 DAG 图校验、执行、线程池、持久化接口和观测事件，不把某一种业务流程写死为唯一工作流。GitHub Actions 的构建与验证编排在 `.github/workflows/`，不是这个执行引擎。

## 已有不同流程

- Agent 提案：`fleet.agent.proposal.v1`，实现位于 `backend/agent/application/FleetAgent/FleetAgent.cpp`。读取证据并形成提案，不直接执行审批后动作。
- Agent 审批：`fleet.agent.approval.v1`，实现位于 `backend/agent/program/FleetAgentCli/FleetApprovalCommand.cpp`。使用现有审核端口，不绕过人工审批和服务端校验。
- 图执行、恢复、容量和失败测试定义自己的独立图，不依赖上述某条流程的名字。

## 为新流程保留的接口

1. 使用 `WorkflowGraph(name, nodes)` 定义图，节点声明自己的依赖、`Effect`、超时、重试次数与执行函数。新图位于所属业务模块，不把业务实现堆进通用引擎。
2. `DagExecutor::run(graph, options)` 接受不同的图；无须改执行器或增加流程名判断。`runId` 区分运行，图名包含业务版本；不要把两个流程的状态混为一条。
3. 需要恢复时使用 `DurableWorkflow::run(graph, revision, options)`；存储通过 `WorkflowJournal` 接口注入。SQLite 是外部适配器，事务和连接不暴露给工具或流程注册 API。
4. 节点捕获的服务、请求和缓冲区必须活到 `run` 返回。执行器负责排空已接纳任务；不要捕获短命对象后把回调交给更长寿命的后台工作。
5. 外部写入节点标记 `Effect::external`；结果不确定时先核对，不能自动当作普通可重试读取。审批职责仍留在业务服务，不能因流程可以注册就自动获得写权限。

此批只补充多流程边界与 CI 扩展约定，不新增未授权业务流程、动态脚本装载器或新的远程执行入口。