# CentOS 与 Ubuntu_ros 的跨机车队验收

这是使用独立 Action 夹具的历史基线。实际 Nav2 规划、控制、生命周期与断线的本轮验收见 [M08 双机端到端](m08-ros-cross-vm.md)。

2026-11-23 使用两台现有虚拟机完成跨主机实验。CentOS 负责 gRPC 协调、任务账本和 Observer。Ubuntu_ros 的 ROS 2 Humble 边缘负责执行同一个后端 C++ DAG，经真实 DDS 调用 `NavigateToPose`。导航结果由独立仿真 Action 提供。

| 主机 | 角色 | 绑定 |
|---|---|---|
| CentOS · `192.168.239.135` | 协调器、SQLite 协调账本、本机模拟边缘 beta | `127.0.0.1:33513` |
| Ubuntu_ros · `192.168.239.128` | ROS 2 边缘 alpha、边缘 SQLite 收件账本、仿真时钟和 Action | `127.0.0.1:17631`；DDS 域 89 |
| 跨机通信 | Ubuntu_ros 直连 CentOS 的 SSH 端口转发承载 gRPC；CentOS 主机公钥经已认证会话核对 | Ubuntu_ros `17631` → CentOS `33513` |

普通业务和现场 ROS 网络各自使用已有环境。实验通过独立进程、账本、凭据及 ROS 域启动。每个边缘只使用自己的凭据，Ubuntu_ros 只获得 alpha 的授权。 [C++ 架构](../../architecture/overview/cpp-systems.md)

## 验收结果

| 项目 | 实测结果 |
|---|---|
| 能力路由 | alpha 与 beta 各完成 5 张工单。CentOS 协调器按站点派发。 |
| 跨机回执 | 200 次混合提交，200 次持久接收，0 错误。P50 18.09 ms，P95 28.54 ms，P99 30.59 ms。 |
| 协调器中断 | Ubuntu_ros 的测试边缘持久化工单并发出接收回执，故意留住未消费的确认。此时强制退出实验协调器。 |
| 断线对账 | 协调器重启后初始状态为 `accepted`、待对账。原边缘恢复后沿同一工单、epoch 和 Trace ID 完成，边缘账本 `starts=1`。 |
| Observer | 从 Ubuntu_ros 导出两条实际 DAG 链路。CentOS 查回 44 条节点与边事件，重复导入后仍为 44 条。 |

ACK 从操作端提交开始计时，到第一次查询到协调器持久化接收为止。混合样本包含本机 beta 和跨机 alpha，发起间隔为每张 100 ms，不能据此推断 Ubuntu_ros 单边 P99。该轮验证覆盖实际两主机传输和合成导航。 [机器可读证据](../../evidence/cpp-systems/20261123/cross-vm.json)

## 用现有隔离目录重跑

以下命令各在自己的终端执行。凭据和 CentOS 主机公钥来自实验私有文件。运行后用新的任务编号，便于看到新建、接收、执行和完成的过程。

### CentOS · 协调器

```bash
lab="$HOME/phase11-data/cpp-systems-20261122.lylnXq"
run="$lab/distributed-20261123.ZGtQTn"
bin="$lab/build/release/fleet/transport/grpc"
export LD_LIBRARY_PATH="$HOME/.local/lib:$HOME/.local/lib64:$LD_LIBRARY_PATH"
env NEBULA_FLEET_OPERATOR_TOKEN="$(cat "$run/operator.token")" \
    FLEET_ALPHA_TOKEN="$(cat "$run/alpha.token")" \
    FLEET_BETA_TOKEN="$(cat "$run/beta.token")" \
    "$bin/nebula_fleet_coordinator" 127.0.0.1:33513 "$run/fleet.sqlite" \
    alpha 1 FLEET_ALPHA_TOKEN beta 2 FLEET_BETA_TOKEN
```

在另开的 CentOS 终端用相同的 `lab`、`run`、`bin` 和库路径启动 beta。

```bash
env NEBULA_FLEET_ROBOT_TOKEN="$(cat "$run/beta.token")" \
    "$bin/nebula_fleet_edge" 127.0.0.1:33513 beta "$run/beta.sqlite" 2 5 "$run/beta.jsonl"
```

### Ubuntu_ros · 跨机通道

```bash
lab="$HOME/nebula-nav2-20261122.aExKhs"
run="$lab/distributed-20261123.38OtTE"
ssh -NT -o StrictHostKeyChecking=yes \
    -o UserKnownHostsFile="$run/centos.known_hosts" \
    -o GlobalKnownHostsFile=/dev/null -o HostKeyAlgorithms=ssh-ed25519 \
    -o ExitOnForwardFailure=yes \
    -L 127.0.0.1:17631:127.0.0.1:33513 pikachu@192.168.239.135
```

### Ubuntu_ros · 仿真 Action 与边缘

两个终端都从下面的环境配置开始，以保证 ROS 域与动态库位置一致。

```bash
lab="$HOME/nebula-nav2-20261122.aExKhs"
run="$lab/distributed-20261123.38OtTE"
grpc_root="$HOME/phase11-deps/cpp-systems/humble-grpc-1.30.2/rootfs/usr"
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="$grpc_root/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
export ROS_LOCALHOST_ONLY=1 ROS_DOMAIN_ID=89
export ROS_HOME="$run/ros-home" ROS_LOG_DIR="$run/ros-logs"
```

第一个终端启动隔离 Action，看到 `SIM_ACTION_READY` 后启动边缘。

```bash
"$lab/build/ros-relocated/nav2_gateway/nebula_nav2_contract" --serve-alpha
```

第二个终端启动 ROS 2 边缘。

```bash
env NEBULA_FLEET_ROBOT_TOKEN="$(cat "$run/alpha.token")" \
    "$lab/build/ros-relocated/fleet/transport/grpc/nebula_fleet_edge" \
    127.0.0.1:17631 alpha "$run/alpha.sqlite" 1 nav2 "$run/alpha.jsonl"
```

### CentOS · 发工单与观测

使用与协调器相同的目录和库路径。`status` 能直接看到工单所归属的机器人、阶段与 `traceId`。

```bash
export NEBULA_FLEET_OPERATOR_TOKEN="$(cat "$run/operator.token")"
task="ros-demo-$(date +%s)"
"$bin/nebula_fleet_ctl" 127.0.0.1:33513 submit "$task" alpha
"$bin/nebula_fleet_ctl" 127.0.0.1:33513 status "$task"
```

实验工单与性能样本保留在 CentOS 的 `smoke-results.json`、`ack-bench.csv`。恢复前后对照见 `cut-accepted.json`、`cut-before-reconnect.json`、`cut-final.json`。Observer 的 `crossvm-observer-proof.json` 在 CentOS 侧，`durable-crossvm-proof.json` 在 Ubuntu_ros 侧。结束时在对应前台终端按 Ctrl+C。

[构建、单机与 sanitizer 复验](cpp-systems.md)
