# 机器人 Skill、能力声明与执行适配 · 业务说明

Skill 定义可复用业务能力的名称、版本、输入输出和执行约束。车队边缘声明能力，协调器能在派发时筛选候选。边缘执行检查资源占用，导航使用底盘互斥。模拟导航和 Nav2 仿真适配通过同一个 NavigationSkill 边界交接。

加入技能时同步代码合同、兼容协议、权限和超时行为，以及 Observer 事件。M08 之后的硬件执行、安全停机和实时控制需要各自的隔离验收。现有已验证路径可在软件边缘或指定仿真环境复现。

M08 Nav2 配置以本体 lifecycle 管理许可。连接租约、ROS 时钟和同一命名空间的 TF 就绪后，边缘才能发出导航 Action。任务记录、ROS Action UUID 与 Observer 链路通过 `traceId` 对照。 [M08 复验](../../ops/validation/m08-ros-cross-vm.md)

[技能合同目录](../../roadmap/embodied-systems/skills/README.md) · [能力及版本实现](../../../backend/fleet/domain/RobotSkills/RobotSkills.cpp) · [NavigationSkill 接口](../../../backend/fleet/ports/NavigationSkill/NavigationSkill.h) · [ROS 仿真适配](../../../robotics/nav2_gateway)

[实操学习](learning.md) · [业务目录](../README.md)
