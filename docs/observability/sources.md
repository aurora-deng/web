# 参考资料与演进顺序

2026-11-20 更新。真实监控栈已在同一 Linux 虚拟机的 A/B 聊天实验中运行。以下版本是该次验收使用的组合，监控制品位于 `/home/pikachu/phase11-deps/observability/`，官方 SHA-256 与启动参数集中在部署页。[版本与部署](setup.md#完整遥测档) · [实测证据](validation.md#双实例实时投递与告警验收)

## 技术用在哪里，怎么验证

| 技术与版本 | 在 NEBULA 的作用 | 目前验证范围与操作入口 |
|---|---|---|
| Prometheus 3.13.5、Node Exporter 1.12.1 | Prometheus 抓业务与监控指标并计算告警。Node Exporter 提供主机 CPU、内存、网络和磁盘，单请求阶段由业务指标和 Trace 提供。 | `metrics` 与 `full` 档已用真实后端。按 A/B 选择实例，对齐 ACK、业务队列与主机时间窗。用 `promtool` 检查低流量、近期 P99 和故障恢复。[Explore 与告警](explore-and-connect.md) |
| OpenTelemetry Collector 0.165.0 | NEBULA 用 OTLP HTTP 导出抽样 span，Collector 接收、批量转发并以有限队列重试。 | `full` 档暂停 Collector 时有 18 次成功业务请求与一次已计数的 Trace 丢弃，恢复后重新读到完整链路。接下来查网关 span 与业务 span 的同 Trace 关联。[断链与恢复](dual-instance-lab.md#3-collectortempo-与测试通知) |
| Grafana Tempo 3.0.3 | 存储和查询按 Trace ID 串联的探针、业务与数据库 span。 | A/B 双实例各回读一条八段链路。当前隔离档单进程本地存储，七天保留，部署页记录 3.0 配置键。[链路下钻](explore-and-connect.md) · [保留设置](setup.md#完整遥测档) |
| Alertmanager 0.34.2 | 将 Prometheus 已判定的告警去重、分组并路由到接收端。Prometheus 负责阈值和持续时间。 | 实验回环接收端已收到 A 的故障与恢复两种通知。真实团队接收端待验收，先通过测试接收端检查去重、路由和恢复通知。[告警条件](dual-instance-lab.md#4-告警口径) · [Alertmanager 官方说明](https://prometheus.io/docs/alerting/latest/alertmanager/) |
| PostgreSQL 18.6 与迁移 025 | 消息和 Outbox 原子提交，投递位置供 A/B 分别消费。 | 已验证临时库中的跨进程交付、进程退出后查历史、原键去重与唯一行数。异机网络及数据库主备待验证。[中继机制](dual-instance-lab.md#1-消息怎么跨进程) |
| Envoy 与 C++ Proxy-Wasm | 原生代理负责入口协议和流量治理，可选 Wasm 计算 WS/SSE 建连响应头耗时。 | 当前为配置、脚本及原生 C++ 契约。制品、SDK ABI、真实代理和插件加载分步验收。[操作手册](envoy-gateway.md) |
| Observer、Grafana、Loki、Jaeger、Pyroscope | Observer 承载中文查询和联动诊断，可导入部分 Grafana JSON。Loki 查外部日志，Jaeger 查 Trace，Pyroscope 查剖析。 | Observer 已用真实 NEBULA 和 Tempo 数据。Loki、Jaeger、Pyroscope 有连接入口及协议验证，真实数据流和联动分别验收。函数墙钟与采样火焰图各有独立统计口径。[数据源与面板](workspace.md) · [函数与内核](resources.md) |
| DAG、Agent 与设备协议 | DAG 记录节点尝试和依赖，Agent 记录工具调用与权限结果，设备记录流序号、ACK 和告警。 | 当前保留模拟器端到端证据，真实生产者接入后用实例、Trace、事件序号及来源标签逐条复核。[接入练习](../roadmap/nebula-cpp-dag-robotics-evaluation.md) |

## 版本更新怎么复验

1. 保留已验组合的版本、制品 SHA-256、配置及一份关键报表。更新前查同一版本的官方发布说明，确认配置字段、HTTP API、存储格式及 Wasm ABI 的兼容性。新增制品按项目的下载、预算和安装审批执行。[制品清单与执行入口](setup.md#完整遥测档)
2. 在新的隔离目录一次替换一个组件。Prometheus 核对抓取标签和规则夹具。Collector/Tempo 核对同一 Trace 的父子关系、故障恢复及留存。Alertmanager 核对触发、去重、恢复。Envoy 核对配置、协议、超时和资源开销。记录启动版本、旧新摘要、失败层和实际回退步骤。[双实例复验](dual-instance-lab.md#2-启动与演练) · [网关复验](envoy-gateway.md)
3. 组件开销对照固定业务二进制、配置、负载和测量时长，记录新旧制品摘要与各自时间窗。日常与隔离环境各有部署记录，变更日常实例前单独验收和授权。短期遥测按七天清理，需长期保存的 PNG、CSV、脱敏报告与版本摘要单独归档。[运行记录](daily-rollout.md) · [留存与导出](ack-diagnosis.md#4-七天保留和停止)

## 从公开方案选出本项目的下一步

| 优先级 | 对服务端开发的价值 | 当前项目怎么做 |
|---|---|---|
| 1 · 量化影响与核对采集 | 先看一个问题影响了多少请求，再决定是否需要处理。 | 本轮补齐三个请求级 SLI、滚动预算、双窗口判定、采集连续性、导出丢弃与同窗 Trace 下钻。六小时策略用于隔离实验。[Google SRE 黄金信号](https://sre.google/sre-book/monitoring-distributed-systems/#the-four-golden-signals) · [SLO 消耗速率告警](https://sre.google/workbook/alerting-on-slos/) |
| 2 · 从请求关联到日志与资源 | 通过同一个请求核对跨进程故障。 | 双实例已经对齐 Prometheus 与 OTLP 实例标识。接着验收网关原生 span 与业务同 Trace，再按实际需要接 Loki、验证曲线 exemplar 下钻。[OTel 资源属性](https://opentelemetry.io/docs/specs/semconv/resource/) · [Grafana Tempo 关联配置](https://grafana.com/docs/grafana/latest/datasources/tempo/configure-tempo-data-source/) |
| 3 · 验证监控自身 | 识别采集缺测与业务错误。 | 现有 Collector 短时故障已验证。接着用接收拒绝、队列容量、导出失败、拉取新鲜度和查询耗时验证完整档长稳与资源预算。[OpenTelemetry Collector 内部遥测](https://opentelemetry.io/docs/collector/internal-telemetry/) |
| 4 · 分离运行时间和等待 | 将尾延迟缩小到 CPU 执行、锁、磁盘、网络或调度。 | 用逐消息分段和同窗 `/proc` 证据，按已有 A/A 噪声判断 A/B 是否可信。按实际瓶颈选择 on-CPU、off-CPU 或 PSI，复现同一个故障验证归因。[Brendan Gregg 的 USE 方法](https://www.brendangregg.com/usemethod.html) · [Prometheus 直方图](https://prometheus.io/docs/practices/histograms/) |
| 5 · 多服务持续运行 | 服务长期运行后仍能定位故障并恢复。 | 先做双上游与异机故障，再扩展实际通知和真实 DAG、Agent、设备生产者。每类保存延迟、数据质量与恢复证据。[下一阶段计划](../roadmap/next-steps.md) |

## 11 月 14 日研讨资料核对

采用 WorkBuddy 同日的《NEBULA监控大盘方案》《NEBULA改进清单》《NEBULA系统开发能力差距》和 `overview.md`。这里记录与现有仓库证据交叉核对后的实现决定，外部笔记保留原文。

| 提议 | 仓库证据与采用方式 |
|---|---|
| 补直方图再做大盘 | 已有 AI 首 Token 和生成时间直方图。把工作集中在会话、历史、聊天处理阶段。 |
| 指标需要 ops token | HMAC 启用时真实测到 401/403/200。监控隔离启动器始终生成独立 HMAC 与抓取凭据。 |
| 只用 Grafana 画图 | 对标 Grafana 的联动与链路下钻。当前自带图表由浏览器原生 SVG 渲染，完整档复用 Prometheus、OTel Collector 和 Tempo。 |
| 先补故障实验 | 复用 [Function3.2 证据](../releases/function3.2.md)，补监控链路隔离、配额、排空和恢复相关检查。 |
| 修改底层 logger 或信号处理 | 本轮在业务层增加结构化记录。冻结门禁继续执行，保留内核告警诊断作为后续议题。 |
| 恢复指标清单和文档索引 | 按当前真实指标列出范围和盲区，使用脚本生成目录并保留阶段历史。 |
