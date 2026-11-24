# 学习入口

结合真实服务学习指标、链路、事件和故障演练，请从 [NEBULA 可观测性实战](../observability/learning.md) 开始。

|学习目标|建议主线|
|---|---|
|理解底层通信|[Reactor课程](../architecture/communication/reactor/lessons/README.md) → [WebSocket课程](../architecture/communication/websocket/lessons/README.md) → SSE/HTTP2/TLS/gRPC|
|理解业务接入|[业务教学目录](../architecture/business/guides/README.md) → [数据库分层](../database/domain-layering.md) → [真实代码索引](../architecture/business/indexes/phase49-code-index.md)|
|按功能读代码、复验权限和故障|[业务说明与学习目录](../business/README.md) → [业务上线台账](../business/release-matrix.md)|
|掌握测试方法|[测试讲义](../testing/learning/README.md)|
|性能定位与故障恢复|[Obsidian 实战计划与可复用助教提示词](nebula-performance-and-reliability.md)|
|C++ 开发、Agent 与具身智能|[总学习方案和模块指导](../roadmap/embodied-systems/README.md)|
|复验 C++ Planner、审批恢复和双 Worker 租约|[v0.4 Fleet Agent 与 DAG 实操](v04-fleet-agent-dag.md)|
|从工单证据到 Agent 建议|[母 Agent、工具与 Skill 实验](nebula-fleet-agent-and-skills.md)|
|练习后端 DAG 恢复、慢流背压与故障回放|[M00–M07 可执行实验](../ops/validation/m00-m07.md)|
|学习 ROS lifecycle、QoS、Action 与跨机租约|[M08 实操](m08-ros-lifecycle.md)|
|学习控制周期、调度长尾与本地看门狗|[M09 实操](m09-actuator-control.md)|
|练习 LangGraph 人工审批与恢复|[StateGraph、检查点与断点恢复](langgraph-nebula-lab.md)|
|练习与自检|[架构习题](exercises/README.md)|
|按开发历史回顾|[阶段时间线](../roadmap/stage-index.md)|

先把一条请求的输入、权限、事务和返回走通，再研究跨模块并发与性能；不要把目录名称当作运行线程或独立进程。
