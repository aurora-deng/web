# 文档索引

按主题分目录。收尾从 **closeout** 读起。

## 目录结构

```text
docs/
  closeout/       收口总述、清单、阶段 A 改动说明
  architecture/   架构、请求流、阶段升级与对照
  testing/        测试说明；learning/ 为学习材料（等你下令再开课）
  performance/    压测方法
  roadmap/        机器人 / 后续方向
  ops/            环境与运维备忘
  archive/        历史阶段材料
```

## 速查

| 主题 | 文档 |
|------|------|
| **收尾总述（先读）** | [closeout/CLOSEOUT_NARRATIVE.md](closeout/CLOSEOUT_NARRATIVE.md) |
| 收口清单 | [closeout/CLOSEOUT.md](closeout/CLOSEOUT.md) |
| 阶段 A（NODELAY/停服） | [closeout/closeout-A_tcp_nodelay_graceful_shutdown.md](closeout/closeout-A_tcp_nodelay_graceful_shutdown.md) |
| 架构 | [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) |
| 请求流 | [architecture/request_flow.md](architecture/request_flow.md) |
| 第三阶段升级 | [architecture/phase3_architecture_upgrade.md](architecture/phase3_architecture_upgrade.md) |
| 代码对照 | [architecture/code_diff_comparison.md](architecture/code_diff_comparison.md) |
| 测试入口 | [testing/TESTING.md](testing/TESTING.md) |
| 测试学习范围（未开课） | [testing/LEARNING_SCOPE.md](testing/LEARNING_SCOPE.md) |
| 学习笔记速查 | [testing/learning/学习笔记速查.md](testing/learning/学习笔记速查.md) |
| 压测方法 | [performance/BENCHMARK.md](performance/BENCHMARK.md) |
| 机器人路线 | [roadmap/ROBOTICS_ROADMAP.md](roadmap/ROBOTICS_ROADMAP.md) |
| 环境备忘 | [ops/environment.md](ops/environment.md) |

## 一键验证

```bash
bash scripts/gen_report.sh
```
