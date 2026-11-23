# Function3.6 发布说明与 Function3.5 对比

对比基线是远端 `Function3.5` 的 `ac2c1dd4b124a7bd4e97ea70821de9bfe566e3ef`。`Function3.6` 包含 C++ 系统基线 `df9fd89`、软件验收提交 `6e82363` 和本轮交付配置。发布标签指向交付提交，`cpp-systems-m00-m07-lab-20261123` 保留原验收提交。 [Function3.5 说明](function3.5.md) · [GitHub 文件差异](https://github.com/aurora-deng/web/compare/Function3.5...Function3.6)

| 范围 | Function3.5 | Function3.6 |
|---|---|---|
| DAG | 小研在浏览器应用层编排偏好与长会话 | 加入后端独立 C++ DAG、Worker 容量、时限、取消和 SQLite 检查点；重启可恢复稳定操作键，未知外部操作进入对账 |
| Agent 与技能 | 小研个性化和具身协作前端样例 | 母 Agent 用独立 CLI 读取工单，产生有界提案，审批与派发职责分离；边缘有 v1 技能和底盘互斥 |
| 车队及 gRPC | 原有站点和网关转发 | 加入独立车队协调器、边缘、能力路由、持久 ACK、重启对账，以及有额外流拒绝计数的负载订阅 |
| 观测与数据 | Observer 增量刷新和真实业务诊断 | 在现有 Observer 中回查车队 Trace、审批与设备记录摘要；验证模拟数据对齐、回放、推理隔离和回滚 |
| 工程与交付 | 两档 Linux 复现，Docker 配方为草案 | C++ 跨平台矩阵及缺测试负向门禁；Linux 可校验 ZIP、离线 Web/Fleet Docker 镜像与部署回退手册 |
| 应用边界 | 聊天、广场、研途和小研 | 延续现有应用入口；车队走独立执行与存储，通信核心文件摘要沿用原值 |

新增代码集中在 `backend/workflow/`、`backend/fleet/`、`backend/agent/`、`backend/systems/`、`tools/agent/` 和对应独立测试。既有 `robotics/nav2_gateway/` 是仿真导航适配。本轮新车队的主要验收在软件边缘完成，真实机器人和实时控制有独立门槛。 [项目目录](../architecture/overview/project-layout.md) · [C++ 调用图](../architecture/overview/cpp-systems.md) · [学习路线](../roadmap/embodied-systems/README.md)

## 测试口径

- CentOS 与 Windows 的独立 C++ Release 各通过 9 项，原生 Debug 各通过 4 项。CentOS 原生 TSan、ASan/UBSan 各通过 4 项。完整 gRPC TSan 仍在现有 gRPC/Abseil 线程栈报警。
- 固定车队负载确认 4,000 次、零失败，P99 为 66.54–71.75 ms，超过 50 ms 参考预算。真实 Qwen 模型命中 13/24，规则命中 24/24，默认仍使用规则。
- Web、Fleet 容器的实际启动、ZIP 内文件、二进制与镜像对应关系记录在 Release 附件 `delivery-verification.json`。选择部署路径前，先核对附件中的架构、修订、迁移和图像 ID。

[M00–M07 复跑及测量](../ops/validation/m00-m07.md) · [结构化实验数据](../evidence/cpp-systems/20261123/m00-m07.json) · [Docker、ZIP 与迁移运行手册](../ops/runtime/function3.6-deployment.md)
