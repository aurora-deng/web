# M08 ROS 2 本体适配、租约与 Action 实操

1. **先看三种状态。** `/alpha/body_agent` 的 `unconfigured → inactive → active` 表示本体受控生命周期。车队 `accepted → executing → succeeded` 表示工单状态。ROS Action 自己还有接受、执行和取消结果。比如工单已进入协调器与边缘账本，仍可能因本体尚未激活而终止。
2. **租约用单调钟。** 协调器每次通过身份与回执校验的 gRPC 回复续期三秒。ROS `/clock` 为目标与 TF 提供仿真时间。把租约看成一张短期通行证，仿真时间暂停也会照常过期。工单 `epoch` 和实际 Action UUID 在运行时各自可查。
3. **分开阻塞等待与回调。** DAG Worker 等待 Action future，ROS Executor 的生命周期和 Action 使用不同回调组。总验收有一个 550 ms 的拥塞对照。观察单线程时的状态空窗，再看两组分离后的回调数。随后核对真实工单仍在执行时，生命周期服务可以停用并取消原 Action。
4. **看 offered 和 requested。** 仿真激光发布端提供 `best_effort + keep_last(1)`。用默认可靠订阅读取时收不到，改成 best effort 后可收到。任务状态和速度 ROS 接口使用可靠传输。仿真器还以本地单调钟清除过期速度，方便区分 DDS 队列配置与本地动作保护。
5. **用 UUID 跟一次断线。** 在总验收的结果文件查 `disconnect.actionId`，查看 ROS 取消结果和协调器 `needsReconciliation`。恢复后查同一个 `traceId`、`epoch`、Edge SQLite `starts`。再提交同一个任务编号，应复用原终态。最后查新工单的 UUID 和结果，复核恢复后的链路。

| 现象 | 先核对 |
| --- | --- |
| 看不到另一个节点 | 当前进程的 `ROS_DOMAIN_ID`、节点命名空间、DDS 发现与本机隔离 |
| 本体在 `inactive` | `body/status` 的 `reason`、租约剩余、仿真钟、TF 和 Nav2 准备度 |
| Action 成功但工单仍显示执行 | 边缘 SQLite、gRPC 流、回执到协调器的确认 |
| 轨迹或动作看起来串车 | 两车的 `/tf`、`/tf_static`、里程计与控制话题前缀，目标 UUID 所属节点 |

[本体状态与权限实现](../../robotics/nav2_gateway/BodyLifecycle/BodyLifecycle.cpp) · [Action 适配器](../../robotics/nav2_gateway/Nav2Navigation/Nav2Navigation.cpp) · [双机复验](../ops/validation/m08-ros-cross-vm.md) · [ROS 2 QoS](https://github.com/ros2/ros2_documentation/blob/humble/source/Concepts/Intermediate/About-Quality-of-Service-Settings.rst) · [ROS 2 回调组](https://github.com/ros2/ros2_documentation/blob/humble/source/How-To-Guides/Using-callback-groups.rst)
