# 应用层可观测性

本目录提供固定操作目录、业务日志、指标和链路追踪支持，不承接 HTTP 路由、数据库业务或进程启停。

| 目录 | 职责 |
| --- | --- |
| `catalog/` | 固定操作名称、标签与声明 |
| `logging/` | 业务日志格式和输出 |
| `metrics/` | 业务指标与文本格式化 |
| `operations/` | 业务操作生命周期和关联上下文 |
| `tracing/` | Trace、Span、聊天 ACK 追踪与遥测 |
| `runtime/` | 提交唤醒等运行时协调工具 |

目录迁移只调整 include、CMake 和工具引用，不改变指标语义、日志字段或资源所有权。`backend/server/` 保持冻结。精确迁移记录见 `docs/architecture/source-application-observability-migration.json`。
