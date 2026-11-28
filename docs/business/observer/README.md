# Observer、监控告警与故障诊断 · 业务说明

业务进程暴露健康和受配置控制的指标。Prometheus 抓取时序指标，Collector 与 Tempo 承担链路接入和查询，Observer 提供数据源、日志、告警、诊断快照、DAG/Agent/设备时间线与图表页面。诊断平台独立存储事件，影响范围与交易数据库分离。

用真实数据区分抓取失败、无采样、零值和业务异常。生产使用需核对授权采集、报告脱敏、留存与配额、真实告警和恢复通知。业务日志默认七天范围由对应运行策略实现。

同一 Observer 进程的告警配置、日志和采集事务共享 SQLite 写入口。写入等待有上限，提交或回滚完成后释放。报表和配置查询走只读连接，告警更新继续核对版本。 [并发写入复验](../../observability/validation.md#配置与诊断日志并发写入)

[Observer 服务](../../../tools/observability/observer/http/server.py) · [架构与边界](../../observability/architecture.md) · [真实验收](../../observability/validation.md) · [日志范围与保留](../../observability/logging.md)

[实操学习](learning.md) · [业务目录](../README.md)
