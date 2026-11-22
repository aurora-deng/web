# 岗位 JD 对照与学习投入

核对日为 2026-11-22。以下是按你的求职和研发目标定向选取的 13 个公开岗位，含 3 个 C++/系统保底对照、9 个机器人岗位和 1 个 Agent 实习对照。表中提炼的是岗位职责与技术信号，投递地点、资格、职位状态以雇主页面为准。

## 一、系统就业与 Agent 对照

| 雇主与岗位 | 样本中的要求 | 对应作品证据 |
|---|---|---|
| 百度 · C++/系统研发/镜像实习生 J71262 | 计算机基础、算法和操作系统，工作方向包含云计算基础技术，也关注系统问题定位。 | M0 的可复现发布与故障报告，M4 的线程生命周期和内存分析。[官方 JD](https://talent.baidu.com/jobs/detail/INTERN/e483ea7e-7698-458b-a58c-99f5f7076bf4) |
| 百度 · 分布式存储研发实习工程师 J71230 | Linux 原理、常用算法与文件系统，参与分布式存储研发。C/C++ 是列出的语言之一。 | M5 的持久账本、单一写入权威、故障恢复，并补操作系统与持久化原理。[官方 JD](https://talent.baidu.com/jobs/detail/INTERN/00f3c88a-75d4-4a30-a040-614e3c4387ea) |
| Canonical · Graduate Software Engineer, Open Source and Linux | 在 Linux 开源栈交付工程成果，C/C++ 属于岗位接受的语言选项。 | 干净检出、可读 PR、诊断记录、Linux 测试与发布证据。[官方 JD](https://job-boards.greenhouse.io/canonical/jobs/8379825) |
| 百度 · Agent 应用开发实习 J105397 | Agent/工作流、知识库、数据治理和自动化评估，评估内容包含执行结果和节点质量。 | M1/M2 的工具轨迹、合法参数、决策复验与故障降级。保留和规则基线的可比较结果。[官方 JD](https://talent.baidu.com/jobs/detail/INTERN/83a5dbcc-48a6-4356-b6d6-b4da9f0a2528) |

## 二、具身智能、软件平台与车队岗位

| 雇主与岗位 | 样本中的要求 | 对应作品证据 |
|---|---|---|
| 智元 · 具身机器人中间件工程师（校招） | Linux 应用框架、工具链、C/C++/Python，以及面向机器人的分布式通信。 | M6 的有界 RPC 流与 M8 的 DDS/QoS 检查，结合开箱就能复验的 ROS 联调。[官方 JD](https://agirobot.jobs.feishu.cn/campusrecruitment/m/position/7661952842403498283/detail) |
| 智元 · 中间件研发实习生 | C++、Linux 调度与内存、传感器时间同步、问题定位。 | M4 并发、M11 时间误差测量、M7 复盘时序。[官方 JD](https://agirobot.jobs.feishu.cn/internrecruitment/m/position/7501886219874322699/detail) |
| DeepMirror · C++ 高级开发工程师（机器人方向） | 现代 C++、ROS 2/DDS、通信与调度、资源性能和软硬件联调。岗位级别可用作进阶对照。 | M6/M8 的延迟和资源证据，M9 的控制周期和设备边界。[企业招聘页](https://www.deepmirror.com.cn/careers) |
| Nimble Robotics · Software Engineer, Robotics Fleet Optimization | 车队多机器人协调与调度软件、算法和系统语言，岗位正文列有 Rust 或 C++ 能力。 | M5 的约束筛选、资源互斥与重连对账。后续用相同基准比对调度策略。[官方 JD](https://job-boards.greenhouse.io/nimblerobotics/jobs/4136334009) |
| Viam · Senior / Staff Software Engineer, Fleet Management | 机器配置、状态和大规模软件交付的控制面，承担从架构到运行的职责。 | 控制面的身份、配置版本、回退和状态对账。作为车队平台的进阶参考。[官方 JD](https://job-boards.greenhouse.io/viamrobotics/jobs/5821871004) |
| Intrinsic · Software Engineer, Data Infrastructure | 机器人高吞吐数据/日志/遥测的采集存储，关注并发、性能和 C++ 加 Python 或 Go。 | M7 的汇总、录包索引、数据质量和断链后的证据回放。[官方 JD](https://job-boards.greenhouse.io/intrinsicrobotics/jobs/6191879004) |
| Atoms · Systems Integration Engineer, HIL | 机器人数据回放、HIL、时钟和自动化，使用 Python、C/C++、Linux 和 ROS 相关工具。 | M11 时序核对与 M13 可复验故障包、软硬件接口测量。[官方 JD](https://job-boards.greenhouse.io/atoms/jobs/8767695002) |
| 沐光机器人 · 嵌入式/机器人底层开发方向岗位 | MCU/RTOS、底层协议、传感器和 ROS 2 驱动、状态诊断与设备保护。 | M9/M10 的本地控制证据、设备档案与安全台架。[企业招聘页](https://www.muguangrobotics.com/careers/) |
| 沐光机器人 · 具身智能/大模型工程师（策略学习方向） | 操作策略、演示数据、模型训练、实机评价和失败反馈。 | M3/M7 的 episode、评估、模型比较与部署证据。该方向安排在平台闭环之后。[企业招聘页](https://www.muguangrobotics.com/careers/) |

## 三、按岗位需要排技术栈

| 学习优先级 | 技术与深度 | 练习办法 |
|---|---|---|
| **必学，D3–D4** | C++17/20 基础、RAII/所有权、并发与原子内存模型、有界队列、Linux 进程/线程/IO、编译链接、CMake 与 GDB/性能工具 | 读已有 Reactor 代码的一条真实路径，构建一个独立调度器；在样本上定位竞态、队列拥塞和 P99 |
| **必学，D3–D4** | RPC、Protobuf、TCP 和 HTTP/2 基础、工作流、数据幂等与恢复、度量和 CI | 用 gRPC 搭控制面，把能力注册、租约、对账和状态流与一次可复现故障写成报告 |
| **具身求职转接，D2–D3** | ROS 2/rclcpp、Action、DDS/RMW、QoS、executor、TF、rosbag2/MCAP、Linux 基线与驱动边界 | 复用任务合同接两台仿真实例，再验证一次 QoS/时钟/失联或设备反馈异常 |
| **AI Native 加分，D2–D3** | 有类型的工具与 Skill、RAG 引用权限、模型调用和离线评估、基于证据的反馈 | 完成母 Agent 对故障的只读分析、提案和批准链，与确定性基线对照 |
| **按岗位深化，D1–D2 起** | PyTorch/LeRobot、ONNX 与端侧推理、实时控制、MCU/总线、HIL | 数据集与策略训练各做一条最小闭环，先验证场景与资源，再选具体实现深度 |

工具语言用 C++ 承担高并发核心与边缘适配，用 Python 做回放、测试、数据处理和评估，页面保持交互与证据展示。基础设施方面，现有 CMake、容器与 Observer 足够支撑首版。分布式算法或设备数量扩大后，再根据新瓶颈选择额外依赖。

## 四、把结果写成能核实的求职材料

1. **C++ 版本**。按一次真实或受控故障讲清入队、调度、锁与生命周期。在简历上写实际执行的环境、采样数、指标及验证范围，附能复跑的提交。
2. **机器人软件平台版本**。讲任务怎样经过协调器、边缘桥、ROS Action，再回到业务账本，解释重复任务和本地停机的处理。
3. **AI Native 版本**。从失败证据到可审查提案。展示工具、权限、结果判据和规则基线，附评测样本。

职位选择采用「先看核心职责，再看附加加分项」。保底岗位已有项目证据时可以准备投递，车队与机器人证据按阶段补齐。[总方案](README.md) · [目标架构](architecture.md)
