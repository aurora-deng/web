# 应用观测目录规范

`backend/application/observability/` 按 `catalog`、`logging`、`metrics`、`operations`、`tracing` 和 `runtime` 划分职责，不以同名类目录重复嵌套，也不把观测工具混入业务模块。

## 边界

- `catalog` 维护固定操作标识；Python Observer 读取同一份定义。
- `logging` 管理业务日志，不处理业务事务。
- `metrics` 管理指标与格式化，不承担采样服务的生命周期。
- `operations` 负责业务操作状态与关联 Span。
- `tracing` 提供 Trace、Span 和 ACK 追踪支持。
- `runtime` 只保留运行时协调工具，不扩展为杂项工具箱。

迁移同步更新 include、CMake、测试、运行包和 Observer 引用。历史迁移清单保持原样，后续合法路径变化通过显式迁移链校验；不得忽略任意路径或重写旧摘要来通过测试。

`backend/server/` 不属于此次整理范围。源码检查不能代替完整 C++ 编译；服务和性能验收以 Linux 隔离构建与运行证据为准。
