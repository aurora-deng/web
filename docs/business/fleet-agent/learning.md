# 车队 Agent、提案、审批与模型适配 · 学习与复验

1. 在隔离车队使用合成工单。按 Agent 讲义依次执行读取、起草、查看提案和审批。把提案当成待签字的工单，记录提案号、版本、引用和审批产生的派发记录。
2. 用起草身份尝试审批，再用批准身份重复同一次确认。核对拒绝和幂等，确认动作只被派发一次。
3. 按 M01 使用无效模型候选，观察技能或证据校验拒绝并走规则路径。从 Observer 回看提案到工单的 trace。
4. 按 LangGraph 讲义比较 Python StateGraph 实验的检查点与 C++ 运行路径。复述等待审批时为何要持久化版本和载荷。

[车队 Agent 与 Skill 教学](../../learning/nebula-fleet-agent-and-skills.md) · [LangGraph 助教实验](../../learning/langgraph-nebula-lab.md) · [小研说明](../assistant/README.md)

[业务说明](README.md) · [业务目录](../README.md)
