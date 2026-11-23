# 机器人 Skill、能力声明与执行适配 · 学习与复验

1. 选导航技能读声明。用 Python 写一个仅校验合成夹具的函数，测试缺字段、版本不支持及成功三种输出。对照 C++ 合同找相同校验位置。
2. 在软件边缘给两张工单安排同一底盘资源，观察互斥、超时与取消，再检查冲突是否被记录成可解释的状态。
3. 运行 `ctest --test-dir "$FLEET_BUILD" -R 'nebula_fleet_contract|nebula_fleet_process_contract' --output-on-failure`。后一个用例需要已构建独立 gRPC 档。比较旧边缘声明缺少可选字段时的兼容结果。
4. 在 Observer 以 `taskId` 与 traceId 核对技能开始、结束和错误。ROS 实验仅用既有 Nav2 仿真环境，先查启动前与取消的预期行为。

[M02 Skill 验证](../../ops/validation/m00-m07.md) · [软件边缘与 M08 路线](../../roadmap/embodied-systems/README.md)

[业务说明](README.md) · [业务目录](../README.md)
