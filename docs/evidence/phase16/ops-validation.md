# Phase 16 Linux 运维验证记录

验证环境：CentOS Stream 9 虚拟机，项目目录 `/home/pikachu/phase11-full-src`，正式 HTTP 端口
18090，隔离测试数据库 `phase11_test`。

## 结果

| 检查 | 结果 |
|---|---|
| Release CTest | 93/93，29.58 秒 |
| Debug CTest | 93/93，29.60 秒 |
| ASan + LSan + UBSan CTest | 93/93，35.07 秒 |
| 迁移状态 | expected=4，applied=4，latest=4 |
| 备份校验 | SHA-256 和 `pg_restore --list` 通过 |
| 隔离恢复演练 | migrations=4，users=59，conversations=39，messages=58 |
| PostgreSQL 停止 | `/health/live`=200，`/health/ready`=503 |
| PostgreSQL 恢复 | Web 进程不重启，第 2 次探测恢复 200 |
| readiness 启动门禁 | 服务脚本启动成功并返回 ready |
| 显式日志轮转 | 优雅停服、gzip 归档、重新就绪均成功 |
| 稳定性 smoke | 15 秒、30 次 liveness/readiness/metrics，0 失败 |

稳定性原始样本见 [soak-smoke.jsonl](soak-smoke.jsonl)。A/B 的完整三轮 JSON、环境和日志分别位于
`phase16-poll/20261006T173245Z/` 与 `phase16-active/20261006T173428Z/`。
