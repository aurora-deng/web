# 后端 C++ DAG 与可恢复工作流 · 学习与复验

1. 把 DAG 看成有先后条件的工单。画出 `A → B/C → D`，写明 B、C 可并行，D 等前置完成。把一条反向边加回图，观察环检测拒绝。
2. 从仓库根运行 `python scripts/project/cpp_systems.py --build-dir build/learning-workflow --config Debug`。然后用 `ctest --test-dir build/learning-workflow -C Debug -R 'nebula_workflow_' --output-on-failure` 定位图验证和检查点用例。先使用已配置的 C++20 和 SQLite 工具链。
3. 按 M04 复跑进程中断。比较恢复前后的 `runId`、`traceId` 和节点操作键，写出纯计算、可幂等写和结果未知动作各自如何恢复。
4. 将 Worker 容量设为 1 和 4 重跑。对比耗时与等待，定位单纯增加线程不能消除的依赖。

[复跑命令和验收证据](../../ops/validation/m00-m07.md) · [Observer 复盘](../observer/learning.md)

[业务说明](README.md) · [业务目录](../README.md)
