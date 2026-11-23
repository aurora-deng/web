# 按业务学习与复验

需要接手一个功能时先读业务说明，确认进程、权限、存储和数据边界，再按学习文档复跑一次正常路径和一次失败路径。统一在隔离环境操作，使用合成数据或临时账号。核对可部署范围和未通过的门槛请查 [业务上线台账](release-matrix.md)。

| 业务 | 说明 | 学习与复验 |
| --- | --- | --- |
| 账号、个人资料与联系人 | [业务说明](identity/README.md) | [实操学习](identity/learning.md) |
| 缘续聊天、群组与消息可靠性 | [业务说明](chat/README.md) | [实操学习](chat/learning.md) |
| 文件、聊天附件与社区媒体 | [业务说明](files/README.md) | [实操学习](files/learning.md) |
| 缘生广场、统一动态、榜单与内容治理 | [业务说明](community/README.md) | [实操学习](community/learning.md) |
| 站点管理、权限与审计 | [业务说明](admin/README.md) | [实操学习](admin/learning.md) |
| 公告、通知偏好与语音 | [业务说明](speech/README.md) | [实操学习](speech/learning.md) |
| 小研、网页推理与私人推理 | [业务说明](assistant/README.md) | [实操学习](assistant/learning.md) |
| 知识库、RAG、模型评测与训练 | [业务说明](model-governance/README.md) | [实操学习](model-governance/learning.md) |
| 研途账号工作区与跨端同步 | [业务说明](study/README.md) | [实操学习](study/learning.md) |
| Outbox、消息队列、重放与缓存 | [业务说明](delivery/README.md) | [实操学习](delivery/learning.md) |
| Observer、监控告警与故障诊断 | [业务说明](observer/README.md) | [实操学习](observer/learning.md) |
| Envoy 网关与环境入口 | [业务说明](gateway/README.md) | [实操学习](gateway/learning.md) |
| Windows Companion 与私人资料 | [业务说明](windows-client/README.md) | [实操学习](windows-client/learning.md) |
| 后端 C++ DAG 与可恢复工作流 | [业务说明](workflow/README.md) | [实操学习](workflow/learning.md) |
| gRPC 车队调度、边缘接收与对账 | [业务说明](fleet/README.md) | [实操学习](fleet/learning.md) |
| 车队 Agent、提案、审批与模型适配 | [业务说明](fleet-agent/README.md) | [实操学习](fleet-agent/learning.md) |
| 机器人 Skill、能力声明与执行适配 | [业务说明](skills/README.md) | [实操学习](skills/learning.md) |

`backend/workflow/` 提供通用 C++ DAG，`backend/agent/` 提供车队 Agent 业务，`backend/fleet/` 提供协调器、边缘与 Skill 执行。三者可以从独立车队交付构建和运行。网页小研与 Windows 私人助手另有自己的交互、上下文和隐私边界。 [后端调用图](../architecture/overview/cpp-systems.md)

[HTTP 业务路由对应关系](../contracts/business-docs.json) · [完整路由契约](../contracts/business-routes.json) · [现有通信核心学习入口](../learning/README.md)
