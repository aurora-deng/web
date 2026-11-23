# 车队 Agent、提案、审批与模型适配 · 业务说明

`backend/agent/` 是独立 C++ 车队业务。Agent 读取工单和允许的证据，规则或可用模型生成候选，C++ 合同校验证据与技能后写入待审提案。审批角色和读取、起草角色分离。经有权确认后，由协调器完成工单派发与状态记录。

实验工具在 `tools/agent/`，LangGraph 指导用于学习状态图和断点审批。网页小研的个人对话及偏好仍由自己的应用流程组织。车队 Agent 上线前应重验权限、审批幂等、重复提交、输出合同、模型失败回退和审计。现有基线以规则为默认。

[Agent 应用](../../../backend/agent/application/FleetAgent/FleetAgent.cpp) · [提案合同](../../../backend/agent/domain/FleetProposal/FleetProposal.h) · [CLI 与审批](../../../backend/agent/program/FleetAgentCli) · [M00–M07 报告](../../ops/validation/m00-m07.md)

[实操学习](learning.md) · [业务目录](../README.md)
