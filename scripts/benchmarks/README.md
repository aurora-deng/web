# 基准测试驱动

本目录只放负载编排与比较，不混入报告展示或端口诊断。目录迁移不改变负载、验收门槛、默认配置或进程清理逻辑，也不自动运行压测。

| 入口 | 职责与边界 |
| --- | --- |
| `benchmark.sh` | HTTP wrk/wrk2 驱动；默认报告适配器为 `scripts/reports/wrk_report.lua` |
| `benchmark_phase10.sh` | HTTP / gRPC 分开采样；保留相对调用者工作目录的 `BUILD_DIR` 与 `RESULT_ROOT` 语义 |
| `compare_phase10_results.py` | 读取两个已生成的汇总文件，检查原有吞吐、尾延迟和错误率门槛 |
| `phase15-business-benchmark.sh` | **会重建专用数据库并停止/启动服务**，必须先核对隔离实例并获得批准；不能当普通单元测试运行 |
| `phase26-capacity-benchmark.sh` | 容量矩阵，复用 Phase 15；保留显式服务重启开关与模型前置检查 |

从项目根目录使用 `bash scripts/benchmarks/benchmark.sh --help` 查看通用驱动参数。Phase 15/26 没有无副作用的帮助入口，不要以 `--help` 为由执行它们。原 `scripts/` 下同名命令只做薄转发，参数、环境与退出码保持传递。

[脚本总览](../README.md) · [报告工具](../reports/README.md)
