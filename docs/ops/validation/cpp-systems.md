# C++ 系统运行与验收

阶段以 2026-11-22 的 Function3.5 工作区为基线。工作区已有安全与网关改动，验收对象是本轮 C++ 系统源码及匹配的测试夹具。运行使用独立 SQLite 文件和回环 gRPC 端口。

M00–M07 的最新测试矩阵、恢复语义和测量口径单列在交付报告。本页下方的早期 2 项 Sanitizer 和跨机结果保留作历史基线。 [M00–M07 软件交付与复验](m00-m07.md)

独立 C++ 系统构建会注册 M09 执行器合约。ROS 档一并构建仿真适配器，故障演练和 Linux 长测由单独脚本触发。 [M09 启动和实测](m09-actuator-control.md)

## 一、按环境构建

需要支持 C++20 的现有工具链、CMake 3.21+、Python 3.9+。Windows 使用系统自带的 `winsqlite3.dll`，Linux 启用车队时需要已有 SQLite 开发头和库。gRPC 可选档要求已有 gRPC C++、Protobuf、`protoc` 和 `grpc_cpp_plugin`。Windows 的 MinGW 与 protoc 组合遇到中文路径时，可使用 ASCII 路径的源码副本、SDK 前缀和构建目录。

```bash
python3 scripts/project/cpp_systems.py --build-dir build/systems-debug --config Debug
python3 scripts/project/cpp_systems.py --build-dir build/systems-release --with-grpc
```

已有 SDK 放在私有前缀时加 `--prefix /path/to/existing-sdk`。Windows 使用 `python`。脚本会核对测试注册数并写入 `cpp-systems-evidence.json` 与 `ctest.xml`。使用完整仓库运行 gRPC 套件，Observer 集成测试还会加载已有的 [操作目录](../../../backend/application/observability/OperationCatalog/OperationCatalog.def)、[路由清单](../../contracts/business-routes.json) 和 Observer 静态资源。

在 Linux 有对应运行库的环境可以分别执行下列档位。启动或链接阶段失败时，从证据的 `phase` 和日志区分环境与测试失败。

```bash
python3 scripts/project/cpp_systems.py --build-dir build/systems-tsan --config Debug --sanitizer tsan
python3 scripts/project/cpp_systems.py --build-dir build/systems-asan --config Debug --sanitizer asan-ubsan
```

CI 的 `cpp-systems` 矩阵包含 Linux Release、Windows Debug、Linux TSan、Linux ASan/UBSan。已预装 gRPC 的隔离 Runner 可手动选择 `run_fleet`，按 `WEB_CI_FLEET_PREFIX` 查找已安装 SDK。Ubuntu Hosted Runner 的 SQLite 开发包可见其镜像清单。 [NEBULA CI](../../../.github/workflows/ci.yml) · [Ubuntu 24.04 镜像清单](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md)

CentOS 验收复用了已安装的 gRPC 和 SQLite。缺失的 TSan 库使用同版本 CentOS RPM 私有解包，RSA/SHA-256 签名和包摘要通过验证。进程以 `LDFLAGS` 和 `LD_LIBRARY_PATH` 引用 `/home/pikachu/phase11-deps/cpp-systems/tsan-11.5.0-15.el9/usr/lib64`。原生 TSan 跑 DAG 与账本两项。选用同样插桩的 gRPC/Abseil 工具链后，再尝试全链路 TSan。

首次 TSan 报告在定时条件变量上出现 GCC 旧运行库已有的双锁诊断。调度等待改用有界信号量合并唤醒，任务期限依然用单调钟。原生两项复验通过。全 gRPC TSan 的另一个报告指向预编译 gRPC/Abseil 的容器与线程启动，保留为工具链待验。 [GCC 定时等待说明](https://gcc.gnu.org/pipermail/libstdc%2B%2B/2023-May/055938.html) · [Abseil Sanitizer 说明](https://github.com/abseil/abseil-cpp/blob/master/FAQ.md#how-do-i-use-the-llvm-sanitizers-with-abseil)

## 二、启动本机演示

自动验证。打开构建输出的 `ctest.xml` 并确认 `nebula_fleet_process_contract` 被执行。该用例起独立协调器、两个边缘和独立 Observer，并覆盖进程强制退出。 [进程用例](../../../tests/cpp_systems/fleet_process.py)

手动验证。进入新建且只允许当前用户访问的运行目录，下例假定已设好 `BIN` 指向产物 `fleet/transport/grpc` 所在目录。为操作端和每个边缘分别生成至少 24 字符随机令牌，使用各自的进程环境注入。协调器得到三种凭据，操作端只得到操作令牌，边缘只得到自身令牌。值保存在你的实验环境里。

```bash
# 协调器终端，设置 NEBULA_FLEET_OPERATOR_TOKEN、LAB_EDGE_A、LAB_EDGE_B 后执行
"$BIN/nebula_fleet_coordinator" 127.0.0.1:0 fleet.sqlite alpha 1 LAB_EDGE_A beta 2 LAB_EDGE_B
```

把该进程打印的 `READY` 地址写入其他终端的 `ADDR`。站点掩码 `1` 对应 alpha，`2` 对应 beta，`3` 对应两者。最后一个参数指定可选事件文件。

```bash
# 边缘 A 终端，NEBULA_FLEET_ROBOT_TOKEN 使用 LAB_EDGE_A 对应值
"$BIN/nebula_fleet_edge" "$ADDR" alpha alpha.sqlite 1 5 alpha.jsonl
# 边缘 B 终端，NEBULA_FLEET_ROBOT_TOKEN 使用 LAB_EDGE_B 对应值
"$BIN/nebula_fleet_edge" "$ADDR" beta beta.sqlite 2 5 beta.jsonl
# 操作端终端，使用 NEBULA_FLEET_OPERATOR_TOKEN
"$BIN/nebula_fleet_ctl" "$ADDR" submit lesson-1 alpha
"$BIN/nebula_fleet_ctl" "$ADDR" status lesson-1
```

`status` 返回 `epoch`、`traceId`、操作者可见阶段和 `needsReconciliation`。为独立 Observer 登记一个 `domain=dag`、`simulated=true` 的来源，将该来源的令牌放入 `NEBULA_FLEET_OBSERVER_TOKEN`。导入后在 Observer 用同一 `traceId` 查询 DAG。

```bash
python3 scripts/project/fleet_observer.py /path/to/alpha.jsonl --observer http://127.0.0.1:18785
```

同一文件可以重放。Observer 根据来源和事件 ID 去重，导出器只连接显式指定的本机地址。结束时在各自前台终端按 Ctrl+C。恢复演练保留该次的三个 SQLite 文件，并用同样的身份配置重启。

## 三、ROS 2 仿真入口

ROS 2 适配以 Ubuntu 22.04 和 Humble/Nav2 为本轮验证环境。场景仿真先启动独立世界与命名空间。动作端为 `/<robot>/navigate_to_pose`，导航状态使用正在推进的 `/clock` 和本地图坐标。实验地图需确认 alpha `(0.5, 0.0)`、beta `(1.0, 0.3)` 可导航。使用隔离的 ROS 域。

ROS 2 环境可以先单独构建导航适配器与 DDS Action 夹具。夹具提供仿真时钟、到站与取消结果。场景仿真再使用实际 Nav2 与地图。

```bash
cmake -S robotics/nav2_gateway -B build/nav2-contract -DBUILD_TESTING=ON
cmake --build build/nav2-contract --parallel 2
ctest --test-dir build/nav2-contract --output-on-failure --no-tests=error
```

整链路验证可使用 `cpp_systems.py --with-grpc --with-nav2`。它会要求两项 ROS 用例被实际注册，并启动独立 DDS 域中的 Action 与时钟夹具。使用 CMake package 或 `pkg-config` 提供 gRPC，`PATH` 指向相同版本的 `grpc_cpp_plugin`。Ubuntu_ros 上私有解包的 SDK 通过实验目录中的 `relocated-pkgconfig/` 指向实际头文件和多架构库目录。发行包和摘要存于 `~/phase11-deps/cpp-systems/humble-grpc-1.30.2/pkgs/`。

Ubuntu_ros 上复验现有产物的命令如下。

```bash
cd ~/nebula-nav2-20261122.aExKhs
source /opt/ros/humble/setup.bash
grpc_root="$HOME/phase11-deps/cpp-systems/humble-grpc-1.30.2/rootfs/usr"
export PATH="$grpc_root/bin:$PATH"
export LD_LIBRARY_PATH="$grpc_root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$PWD/relocated-pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export ROS_HOME="$PWD/ros-home" ROS_LOG_DIR="$PWD/ros-logs"
python3 -B scripts/project/cpp_systems.py --build-dir build/ros-relocated --with-grpc --with-nav2 --prefix "$grpc_root"
```

```bash
source /opt/ros/humble/setup.bash
export ROS_DOMAIN_ID=37
cmake -S backend/systems -B build/systems-nav2 -DCMAKE_BUILD_TYPE=Release \
  -DNEBULA_FLEET_GRPC=ON -DNEBULA_FLEET_NAV2=ON
cmake --build build/systems-nav2 --parallel 2
ros2 action info /alpha/navigate_to_pose
# BIN 指向 build/systems-nav2/fleet/transport/grpc。边缘 B 继续模拟。
"$BIN/nebula_fleet_edge" "$ADDR" alpha alpha.sqlite 1 nav2 alpha.jsonl
```

用隔离账本分别验证到站、无仿真时钟、动作拒绝与超时取消。根据工单状态和 ROS Action 结果核对同一任务。Action 夹具使用真实 ROS 2 传输和模拟导航结果。实际 Nav2 地图与物理运动需要单独验收。 [Nav2 Humble Action 定义](https://github.com/ros-navigation/navigation2/blob/humble/nav2_msgs/action/NavigateToPose.action) · [rclcpp Action Client 接口](https://github.com/ros2/rclcpp/blob/humble/rclcpp_action/include/rclcpp_action/client.hpp)

## 四、性能口径

`nebula_workflow_contract --benchmark` 测五节点图完整运行，每种 worker 配置跑三轮，每轮 1,000 次。事件输出关闭。ACK 基准启用 `-DNEBULA_FLEET_BENCHMARK=ON`，使用 `nebula_fleet_ack_bench ENDPOINT COUNT ROUNDS PERIOD_MS`。在运行中的独立测试车队测量，样本自操作端发出 Submit 起，到第一次读到协调器已持久化接收的状态为止，包含状态轮询等待。它不同于机器人完成导航的总时长。使用壳脚本 [fleet_process.py](../../../tests/cpp_systems/fleet_process.py) 的 `--benchmark` 档可从空账本复跑，提供正式二边缘与三边缘曲线。

CentOS Stream 9 虚拟机使用 4 个 vCPU、GCC 11.5、SQLite 完整同步、单张在途提交和 5 ms 模拟行程。关闭事件文件输出，每 100 ms 发一张工单。以下均为调整定时等待后的样本。

| 边缘数 | 轮次 | 接收成功 | 错误 | P50 | P95 | P99 |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 1 | 1,000 | 0 | 17.22 ms | 28.94 ms | 33.66 ms |
| 2 | 2 | 1,000 | 0 | 17.26 ms | 28.55 ms | 33.45 ms |
| 2 | 3 | 1,000 | 0 | 17.97 ms | 29.23 ms | 37.52 ms |
| 3 | 1 | 1,000 | 0 | 13.66 ms | 24.58 ms | 30.88 ms |

双边缘约 9.95 次接收/秒，三轮 P99 均低于初始 50 ms 预算。三边缘只有一轮，保留为扩展样本。五节点空工作图的 1/2/4 worker 三轮 P99 分别为 1.01–1.75 ms、1.33–1.41 ms 和 1.10–1.41 ms，峰值就绪节点均为 2。该图支路只做短计算，增加 worker 的收益需要在真实技能负载下衡量。

## 五、故障证据和发布边界

- **跨进程**。Linux 和 Windows Release 均完成 30 张按能力分配的工单，以及 20 次持久化接收回执在边缘确认前中断。边缘收件账本逐条核对为启动一次。协调器崩溃后沿同一 `taskId`、`traceId`、`epoch` 恢复。执行中强杀的工单标为 `uncertain`，后续工单留在队列。Observer 复核真实 C++ 产生的 22 条图事件并验证重放去重。
- **ROS 2**。`Ubuntu_ros` 的 Humble Release 整链路 5/5。协调器同时连接普通模拟边缘和 ROS 2 边缘，ROS 边缘通过真实 `NavigateToPose` Action 完成 5 张仿真工单，Observer 可按工单 Trace 回查。两项 ROS 用例各重复三遍并通过。
- **线程工具**。Linux 原生 TSan 2/2，ASan/UBSan 2/2，覆盖线程退出、取消、迟到完成和 SQLite 恢复。已有 SDK 下的全 gRPC TSan 保留 Abseil 诊断。Release 联调另行通过。
- **复现位置**。CentOS 隔离目录为 `/home/pikachu/phase11-data/cpp-systems-20261122.lylnXq`，包含 `build/*/ctest.xml`、`fleet-bench-*.csv` 和源码归档。Ubuntu_ros 目录为 `/home/pikachu/nebula-nav2-20261122.aExKhs`，包含独立和整链路构建记录。
- **版本定位**。本地复现标签 `cpp-systems-pre-agent-20261123` 从 `ac2c1dd` 延伸，包含 C++ 系统模块、CI 配置和配套文档。快照与跨机验收使用同一份 C++ 源码摘要。摘要按区分大小写的路径顺序计算，先统一换行符，因此 Windows 和 Linux 可直接对照。站点发布单独验收。

[架构与故障边界](../../architecture/overview/cpp-systems.md) · [两台虚拟机实测](cpp-systems-cross-vm.md) · [机器可读验收摘要](../../evidence/cpp-systems/20261122/summary.json)
