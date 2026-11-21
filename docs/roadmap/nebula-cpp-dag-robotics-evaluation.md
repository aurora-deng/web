# NEBULA 工程评估与 C++、DAG、具身智能路线

## 1. 当前能力和证据边界

| 方向 | 当前能展示 | 下一份最有价值的证据 |
|---|---|---|
| C++ 系统开发 | 自研 Reactor、HTTP/WS/SSE。可按队列、事务和 ACK 诊断行锁，并在双进程直连实验中复核跨实例投递、节点退出后的历史和唯一写入 | Envoy 单入口的跨协议实测，再补双上游切换、容量曲线与资源限额 |
| SRE 与研发诊断 | 自建中文 Observer，真实 Prometheus、Tempo 链路回读、Collector 故障恢复和本机 Alertmanager 通知 | 让第二个服务复用接入方案，验证网关到业务的同 Trace、持续积压和跨主机实例标识 |
| DAG | `dag.run`、`dag.edge`、`dag.node` 契约，模拟并行汇合、重试和跳过，前端拓扑与甘特图 | 两个真实 Worker 接手同一个可恢复的 DAG。Worker 重启、重复提交、超时、取消和恢复都留下可追溯状态 |
| 具身智能 | 设备会话、流窗口、序号、指令和 ACK 关联的模拟遥测；记录缺帧、乱序和时钟可信度 | 回放一份带版本和时基的 rosbag2/MCAP，让真实 ROS 2 订阅器产出窗口指标，Observer 能回到同一片段 |

整机容量、多机容灾、真实 DAG 执行和设备控制各自验收。先用隔离报告理解进程恢复，再加网关和第二台主机，逐步扩大故障范围。[业务构建与复现](../evidence/function3/project-review-20261119/summary.json) · [双实例实测](../observability/validation.md#双实例实时投递与告警验收) · [事件契约](../observability/architecture.md#事件写入与查询)

## 2. 参考成熟项目时看什么

1. **Envoy**。从一次请求看 HTTP Connection Manager、过滤器和上游连接池。读完能解释谁创建请求 ID、何时使用连接、超时由谁触发，随后用一次 504 的日志和业务 Trace 验证。源码阅读可以只沿同一条链进入两个组件，实验配置和结果放一起。[连接管理架构](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/http/http_connection_management) · [隔离启动](../observability/envoy-gateway.md)
2. **Argo Workflows 与 Temporal**。Argo 的 DAG 示例适合学习“哪些任务可以并行、何时汇合”。Temporal 的事件历史和 Activity 重试适合学习“进程重启后怎样继续，以及任务可能再次执行”。从两者各拿一个可验证的机制，首个小实验保持 `A → B/C → D` 四类固定任务。[Argo DAG](https://argo-workflows.readthedocs.io/en/latest/walk-through/dag/) · [Temporal 事件历史](https://docs.temporal.io/encyclopedia/event-history) · [Activity 幂等](https://docs.temporal.io/activity-definition)
3. **ROS 2、ros2_control 与 Foxglove**。ROS 2 QoS 管理传感器与订阅者能否匹配、保留多少和是否可靠送达。Controller Manager 的更新频率、周期抖动与超限是本地控制需要观察的量。用 MCAP 保存回放文件，用 Foxglove 检视原始相机和传感器，用 Observer 按时间、设备会话和指令标识呈现汇总及故障。[ROS 2 Jazzy QoS](https://github.com/ros2/ros2_documentation/blob/jazzy/source/Concepts/Intermediate/About-Quality-of-Service-Settings.rst) · [Controller Manager](https://control.ros.org/jazzy/doc/ros2_control/controller_manager/doc/userdoc.html) · [Foxglove Bridge](https://docs.foxglove.dev/docs/visualization/connecting/live/ros-foxglove-bridge) · [MCAP](https://mcap.dev/spec)

## 3. 三个可以逐项验收的练习

### 一次入口故障和一次慢 ACK

现在可直接复验行锁慢 ACK。长尾消息按 Trace 找到等待的 PostgreSQL 命令，恢复后核对 ACK 和同键只写一条。Envoy 原生联调后，在同一隔离版本跑 HTTP、WS、SSE 和文件，再观察一次上游 504，通过 `requestId` 确认入口发出超时。保存版本、时间窗、输入和时序，复盘两种故障影响了哪些请求、哪些阶段仍然健康。[慢 ACK 讲义](../observability/ack-diagnosis.md) · [网关手册](../observability/envoy-gateway.md)

### 两个 Worker 完成一张 DAG

- 把 `runId` 当工单，`nodeId` 当工序，`attempt` 当第几次试做。按有向边判断就绪。工序先进行短数据库事务领取租约，再在事务外执行。有效租约的持有者写回结果，过期 Worker 的迟到结果要拒绝。
- 按 `(runId, nodeId, attempt)` 记录每次尝试。涉及外部副作用时用跨尝试稳定的业务幂等键。队列和重试预算有限，失败分支、超时、跳过、取消有各自状态。算关键路径时用各节点的排队与执行时间，B/C 并行的耗时按实际依赖计入。
- 验收时暂停一个 Worker，另一个在租约过期后接手。检查 D 只在前置满足时执行，重复尝试在 Trace 中可见，同一成功副作用只写一次。随后执行一次取消，验证停止派发下游任务和回收资源。首个生产者可用 C++ 任务执行，保留与现有 Observer 通用的版本化事件契约。

### 真实 ROS 2 回放与网络抖动复盘

- 用一份录好的数据包作为固定输入。在独立 ROS 2 观察进程按设备会话和流维护序号水位。读取采集时间、接收时间和基准时钟质量，窗口内分别累计缺帧、迟到、重复及重启。90 秒可复核片段足以验证首个接入。
- 以低频窗口和关键状态向 Observer 上报，附上 MCAP 记录 ID 及流定位。高频数据占用由录制工具管理；Observer 上报队列满时统计丢弃。验收以可重放、可定位、采集故障时订阅端仍健康为条件。
- 固定采样率后，分别给回放增加有界 CPU/RSS 压力和延迟，核对尾延迟同时发生的运行队列、网络丢包与资源等待。观察侧只订阅、统计和记录，实际控制环与急停由本地控制系统负责。任务 DAG 处理跨步骤编排，控制周期和在线反应留在机器人栈。[Linux PSI](https://docs.kernel.org/accounting/psi.html) · [机器人 C++ 学习路线](learning/ROBOTICS_ROADMAP.md)

## 4. 自检与助教提示词

1. 一个 WS 连接用了两小时，为什么建连响应头、消息 ACK 和连接寿命要分成三份数据？
2. 两次 DAG 尝试都执行过，如何保证一个外部写操作只产生一次效果？
3. 相机帧时间与主机接收时间相差 800 ms，先查时钟还是网络，哪几项证据可以区分？

> 请结合当前 NEBULA 源码和本次实验报告教我系统开发。先让我选择入口故障、DAG Worker 恢复或 ROS 2 回放之一。核对版本、日志、时间窗和数据源，再编号讲一条实际调用或状态变化。用简短类比和必要的 C++ 片段解释连接池、租约、幂等或时基。每讲通一层，让我用数据复述判断，等我下一步命令再深入。区分已经实测的数据、模拟数据和仍需接入的部分。所有故障演练都用隔离目录和实验进程。
