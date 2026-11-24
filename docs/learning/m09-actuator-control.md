# M09 实操 · 从导航速度到模拟轮子

跑完这篇后，你应该能解释控制线程为何按单调钟运行、漏拍如何统计，以及控制进程被暂停时，谁还能把速度清零。

## 1. 先把三层职责分开

- **CentOS 的车队协调器**安排任务并续租。它决定哪台机器人接单，业务账本保留任务与回执。
- **Ubuntu_ros 上的 Nav2 与本体生命周期节点**分别计算目标速度、确认当前任务是否有运动许可。任务失去许可时，本体取消对应的 Action。
- **Ubuntu_ros 上的本地执行器和仿真驱动**决定这一拍实际采用多大速度。执行器读取许可、目标和反馈，在本地算限幅。单独进程里的仿真驱动再次限幅，并在目标到期时自己停下。

可以把许可想成有有效期的通行证，把速度目标想成当班的操作指令。持有通行证还要收到新指令才能动作。上一次任务的指令留在队列里也不能交给下一次任务。

## 2. 看懂 100 Hz 的一拍

100 Hz 表示每隔 **10 ms** 预定一次工作。计划时刻来自统一的单调时间轴，每轮仍然用实际读到的时间判断数据有没有过期。

```cpp
const auto release = schedule.nextRelease();
sleepUntil(release);
const auto wake = monotonicNow();
const auto command = commands.takeLatest();
auto target = controller.update(wake, inputs, command);
const auto timing = schedule.complete(wake, monotonicNow());
```

- **计划唤醒时刻 `release`**。提前排好的时间格。
- **实际唤醒时刻 `wake`**。线程真正拿到 CPU 的时间。差值 `wake - release` 是唤醒迟到。
- **执行耗时**从醒来到这一轮完成。**完成延迟**从计划时刻算起，等于唤醒迟到加执行耗时。
- **跳过的时间格**单独计数。如果计划在 110 ms 醒来，136 ms 才做完，120 ms 和 130 ms 两格已经错过，下一次等到 140 ms。连续补跑会让旧指令突然挤在一起。
- `CLOCK_MONOTONIC` 用在单机的期限判断。日志展示可用墙钟。在同一 Ubuntu 内核上，仿真适配器约定共享单调钟；CentOS 到 Ubuntu_ros 只传任务和续租消息，各自计算本地期限。 [Linux 绝对睡眠接口](https://man7.org/linux/man-pages/man2/clock_nanosleep.2.html)

## 3. 数据如何交到控制线程

ROS 执行器线程是消息的生产者，100 Hz 循环是唯一消费者。一条交接队列只有八个槽位，消费者只取已发布的最新快照。队列满时，生产者丢弃这一次新样本并累计计数。旧快照的有效期保持原值。

紧急停止和撤销许可单独保留。这样在消费端来不及读队列时，仍可在下一拍看到停止请求。控制线程内只处理固定大小数据，不等待 gRPC、ROS 发布、文件或日志。诊断由 ROS 回调发出。 [SPSC 实现](../../robotics/actuator_control/SpscQueue/SpscQueue.h) · [线程装配](../../robotics/actuator_ros_sim/ActuatorRuntime/ActuatorRuntime.cpp)

## 4. 把两层限制分别验证

| 试验 | 动手观察 |
|---|---|
| 目标设为 2 m/s 与 3 rad/s | 第一层按每拍的加速度上限改变速度，最后保持在 0.35 m/s 与 1.3 rad/s |
| 持续发送目标，暂停 C++ 控制进程 | 查看仿真体的里程计。驱动进程依据上一帧的本地过期时间自行归零 |
| 测试夹具直接向仿真驱动送越界速度 | 第一层被绕过后，仿真驱动仍限制在 0.4 m/s 与 1.5 rad/s，并在 80 ms 档的看门狗期限内失效 |
| 停止发反馈，随后请求复位 | 复位条件仍不够。恢复健康零速反馈后，再显式复位并用新的任务许可起动 |
| 核对命令超时前后两帧 | 页面或日志中的停止观察时间包含采样与消息传播延迟。对照程序截止时刻再解释差值 |

这里的电机是软件模型，采用即时速度反馈。真实电机还有惯性制动、硬件急停、驱动故障码和电源限制。学习 ROS 硬件适配时，先在 Controller Manager 中找 `read → update → write` 和硬件生命周期，再去读对应设备厂商接口。 [Humble Controller Manager](https://control.ros.org/humble/doc/ros2_control/controller_manager/doc/userdoc.html)

## 5. 拿自己的数据做一次定位

从 [M09 操作与验收](../ops/validation/m09-actuator-control.md) 复跑短测，打开 CSV 找到 `wakeLateNs` 最大的一行。分别回答这三点。

1. 线程是醒得晚，还是醒来之后算得慢？对照 `wakeLateNs` 和 `executeNs`。
2. P99 看起来小，仍有一次特别大的漏拍，设备准入如何写？保留最大值和漏拍，说明受影响的预算与停机动作。
3. 同核负载让这台虚拟机唤醒延迟变小，能否证明 CPU 空闲状态就是根因？当前实验只展示相关性。下一项实验应分别控制虚拟机时钟事件、宿主调度和 CPU 空闲设置，每次只改变一项。 [Linux 实时与虚拟化](https://docs.kernel.org/core-api/real-time/index.html)

面试可以用一句话讲清你做的事。你把云端工单和本地运动周期拆开，通过可复现的故障试验证明本地在输入失效时会停，并用分阶段时序数据定位 100 Hz 的瓶颈主要出现在唤醒阶段。
