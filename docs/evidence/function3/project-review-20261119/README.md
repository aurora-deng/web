# 2026-11-19 全项目改进：选定验收证据

[汇总](summary.json) · [验证说明](../../../observability/validation.md#2026-11-19-全项目改进意见落地) · [实施对照](../../../roadmap/project-review-implementation-2026-11-19.md)

- [原生 Release](reproduction.json)、[空目录通信档](communication.json)、[Debug/UBSan](debug-ubsan.json)、[交付清单](release.json)。
- [真实指标与中断恢复](metrics.json)、[业务请求埋点](business-live.json)、[慢 ACK](ack-diagnosis.json)、[数据库断连及强杀恢复](faults.json)。
- [测试 Provider 的 OTLP 生命周期](model-wire.json)、[安装与摘要](backend-install.json)、[原服务保护核对](protection.json)。
- 选定页面：[函数量化](actual-function-times.png)、[神树数据库调用链](tree-business-sql-trace.png)、[手机显示](business-functions-mobile.png)。

这些是主动选定的脱敏验收报告与图片；不是连续遥测原始库。账号为隔离合成数据，模型 Provider 是测试替身。两种构建的开关、样本和时间窗分别记录；无真实评估集、真实多机后端及长稳证据的项目不标为完成。临时遥测默认七天清理，导出材料独立留存。
