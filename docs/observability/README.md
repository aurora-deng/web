# NEBULA 可观测性

11 月 20 日，同一 Linux 虚拟机上的双实例直连实验已验证真实 Prometheus、Collector、Tempo 及本机 Alertmanager。包含跨实例投递、Collector 暂停恢复、告警通知和同键重试。短时验收的进程已回收，报告留在实验目录。[启动与复验](dual-instance-lab.md) · [验收结果](validation.md#双实例实时投递与告警验收)

Observer 用于跟踪 NEBULA 服务的请求、调用链和资源开销。Windows 通过浏览器操作，Linux 运行隔离业务与采集组件。建议以一个已知故障为主线学习，从用户影响、请求路径一路查到 PostgreSQL、线程或内核。

2026-11-19 已经授权启用日常 18090 的业务采样。18791 查看原聊天室、苦情树的真实监控；18793 保持为独立实验入口。原账号登录和只读业务链路已验证，[启用记录](daily-rollout.md)包含备份、启动、查询和回退步骤。

## 按问题找入口

| 你要回答的问题 | 从哪里开始 |
|---|---|
| 服务没启动、业务未就绪，还是采集有问题？ | 「运行总览」的三类健康项。「服务目标与采集质量」还能核对 SQLite 写入、Trace 导出、丢弃和队列满。 |
| 指标空白，是缺流量、缺埋点还是查询失败？ | 查看图内原因及「检查 → 查询详情」。总览、函数和指标目录共用[诊断规则](architecture.md#空白指标与查询失败)，读取超时后显示重试建议。 |
| 报错在哪个组件，能否关联到一次请求？ | 「日志工作台」筛级别、组件和 Trace ID，支持实时跟随及调用链互跳。[日志与查询编辑器](logging.md)解释七天清理、代码位置和接入方式。 |
| 这段时间影响多少请求，值得马上处理吗？ | 「服务目标与采集质量」看样本、未达标数量、滚动预算和双窗口消耗速度。 |
| 延迟、吞吐、错误、饱和度哪一项先变？ | 「业务性能」对齐时间轴，查看 QPS、尾延迟、业务 5xx、执行器和连接池。用「检查」核对样本和导出 CSV。 |
| 一条真请求具体慢在哪？ | 从指标筛同窗 Trace，或者在「函数与内核剖析」点「看调用链」。排查 PostgreSQL 提交长尾时，可在「提交诊断」打开[实测报告和逐条等待样本](resources.md#在-observer-复盘)。 |
| 修复到底有无收益？ | 「研发实验」比较同环境、同负载的报告，结合 A/A 噪声、样本数量和错误判定。 |
| 接入服务、扩展工作流或硬件？ | 「数据源」「自定义仪表盘」及「扩展遥测」。DAG、Agent、设备样例始终带模拟标记。 |

## 阅读顺序

1. [启动、停止与接口验证](setup.md)
2. [运行拓扑、代码分层与信号契约](architecture.md)
3. [服务端排障课程和可复用助教提示词](learning.md)
4. [指标查询和 Trace 下钻](explore-and-connect.md)与[主机、进程、函数和内核实验](resources.md)
5. [数据源、仪表盘与跨服务排查](workspace.md)
6. [结构化日志、查询编辑器与函数耗时](logging.md)
7. [技术应用、版本与更新验收](sources.md)、[验证结果](validation.md)和[Grafana 对照](grafana-rd-matrix.md)
8. [26 项业务埋点与异步计时讲义](business-instrumentation.md)：操作目录、结果计数、跨线程父子链路及真实复验。
9. [Envoy 入口网关隔离实验](envoy-gateway.md)涵盖代理、入口日志、业务 Trace 关联和 C++ Wasm 对照。实机状态见[网关验收](validation.md#envoy-入口网关隔离验收)。
10. [双实例聊天与真实遥测验收](dual-instance-lab.md)包含 PostgreSQL 实时转发、按实例计算服务目标、Collector 故障和测试通知。[下一阶段计划](../roadmap/next-steps.md)说明 ACK 长尾、双上游与异机的过关标准。

本轮新增埋点先在独立 18797 验证，日常 18090 保持上一版已授权程序。是否启用新版本以[日常记录](daily-rollout.md)为准。全项目改造的本轮交付见[实施清单](../roadmap/project-review-implementation-2026-11-19.md)。

轻量档用内置采集。实测的 `metrics` 档用 Prometheus 和 Node Exporter、保留本机 TraceIndex，可用 18798 复现慢 ACK 教学。`full` 档再使用 Collector 和 Tempo。Observer 按当前数据源读取原始计数计算服务目标，六小时策略用于隔离教学。扩展遥测模拟预览保留来源标记。[启动说明](setup.md) · [慢 ACK 操作与学习](ack-diagnosis.md)

[返回项目文档](../README.md)
