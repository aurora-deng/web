# Observer、监控告警与故障诊断 · 学习与复验

1. 从项目根运行 `python -m tools.observability.preview --data-dir build-function3/observer-learning --port 18785`。使用新的临时目录。页面显示模拟来源，用它学习告警到时间段、Trace 和函数墙钟趋势的下钻。
2. 按双实例实验连接真实抓取目标，用一次慢 ACK 解释排队、PG 租借连接和提交耗时，用 P99、样本量和错误率对比恢复前后。
3. 在隔离环境断开遥测后端，分别查看服务存活、业务就绪、指标抓取和链路缺段。恢复后检查是否有丢弃计数。
4. 核对七天保留和导出证据。用精简 traceId、窗口和摘要形成一次可以被别人复跑的故障记录。

[观测六课实操](../../observability/learning.md) · [数据源实践](../../observability/explore-and-connect.md)

[业务说明](README.md) · [业务目录](../README.md)
