# Observer 与 Grafana 的能力对照和 RD 验收

以 Grafana OSS 的主要能力为对照，单列 Enterprise 和 Cloud 相关能力。对应项以实际数据、操作和测试证据评估。

| Grafana 能力 | Observer 对应入口 | 当前边界 |
|---|---|---|
| Dashboard 与时间序列 | 运行总览、业务性能与自定义仪表盘，支持折线、面积、数值、仪表和表格 | 可从模板创建、增改排列和导入导出 JSON。自由拖拽、版本历史及第三方图表插件待补。 |
| 时间范围、变量、刷新、缩放 | 工具栏、事务和实例、共享游标、拖选缩放、固定窗口、刷新间隔 | 自定义面板支持常用集群、命名空间、服务、实例等参数。任意 Grafana 模板变量仍需适配。 |
| Panel Inspector | 检查抽屉，统计、样本表、查询明细及 CSV | 浏览器按已选序列导出。面板 JSON、转换编辑器、查询历史、快照与计划报表待补。 |
| Explore Metrics | 指标探索、自定义面板与 A–D 查询编辑器 | 支持 Builder/Code、复制、显隐、图例、步长、Range/Instant、表格和直方图。本机执行实际登记桶，完整 PromQL 交给外部 Prometheus；自动补全待补。 |
| Explore Traces | 链路追踪和分布式排障，按 TraceQL 或 Trace ID 搜索 | 真实路由、C++ 业务与仓储、SQL 客户端以及本机 Python 调用树。支持 span 筛选、函数直接下钻、直达链接、本地 HTTP 4xx/5xx 筛选和时间关联。跨请求服务地图、exemplars 和多机 Tempo 待验。 |
| Explore Logs | 独立日志工作台、C++/Observer 结构化日志及可配置 Loki | 本机真实错误、进程线程、源码行、Trace 互跳、稳定分页、连续刷新、JSON 导出和七天清理已接通。实际外部 Loki 及多机汇聚待部署验收。 |
| Explore Profiles | 主机与进程、Linux 内核计数与绑核对照、函数墙钟、Callgrind 指令及 DHAT 堆调用栈；Pyroscope 火焰图 | 本机真实实验通过，Pyroscope 与 Jaeger v3 使用协议替身。真实持续采样及生产集群待接入。 |
| Alerting | 固定与自定义规则、试算、暂停、状态历史和时间窗下钻 | 实测两条固定业务规则及自定义规则触发。分组、静默和通知通道待补。 |
| 服务目标 | 轻量档请求级目标、差量、覆盖、6 小时预算、双窗口消耗速度和同窗下钻 | 教学目标限共享 HTTP 适配器、会话处理和 ACK 就绪。完整 Prometheus 档接入与告警通知待验。 |
| Annotations | 系统实验标线及离线写入的故障/版本标记 | 时间范围与实验报告可查询。浏览器写入审批及共享编辑待补。 |
| Data sources / Plugins | Web 管理 Prometheus、文本指标、HTTP 探测、Tempo、Loki、Jaeger v3 与 Pyroscope | 支持 URL 测试和保存，网络范围和认证由运维配置，第三方插件及数据源细粒度权限待补。 |
| Users / Teams / RBAC / SSO | 回环监听经 SSH 访问；事件写入绑定来源凭据 | 暂无浏览器账号、角色、租户、组织、审计与 SSO。多人生产环境须补齐访问治理。 |
| Provisioning / HA / Backup | 本地 Git JSON 单向同步、校验、稳定版本；JSON 文件导入及本机引用重绑 | 双向 Git Sync、多实例容灾、跨机保留与恢复演练待补。 |
| Grafana Enterprise / Cloud | 当前聚焦开发诊断与 NEBULA 实验 | Enterprise/Cloud 的托管、报表、细粒度权限、插件服务及其他产品模块未对等。 |
| NEBULA 扩展 | 实验复盘、DAG 甘特图、Agent 尝试/Token、设备序号和回执 | 运行真实隔离的会话探针和压测；DAG、Agent、设备演示来自标记的模拟生产者，真实业务适配待接入。 |

## RD 操作矩阵

| 板块 | 验收路径 |
|---|---|
| 启动、错误反馈 | 检查首页、CSS 和所有 JS MIME。用旧路由模拟模块 404，页面显示恢复说明。 |
| 运行总览 | 切换时间窗、自动刷新、实例筛选。核对服务存活/就绪与抓取状态。 |
| 业务性能 | 切换事务、悬停同步、拖选、面板放大、查看统计和查询、下载 CSV；确认 0 和无样本可区分。 |
| 指标探索 | 选线程或会话 P99、查询统计、检查表达式。 |
| 服务目标与采集质量 | 看真实分子分母、采集覆盖和两组消耗速度。用 5xx 过滤跳 Trace；区分无样本、连续无请求、低流量和采集断开。 |
| 链路追踪 | 罗列真实会话探针、筛选并下钻，核对探针 → NEBULA → PostgreSQL。 |
| 主机与函数 | CPU 图跳到同一时间窗，查看主机、进程、线程、调度与 TCP，再展开真实绑核、syscall、函数指令和堆调用栈。 |
| 研发实验 | 展开真实报告摘要，选择两个兼容实验，比较 ACK P99。检查选择不兼容报告时的提示。 |
| 扩展遥测 | 验证 DAG 重试、Agent 统计、设备回执及真实服务事件区分和跳转。 |
| 故障与数据 | 核对 HTTP 契约、去重拒绝、资源一致性、保存后重新打开、监控栈边界。 |

桌面和移动交互、真实模拟来源隔离、以及实际结果见[验证记录](validation.md)。完整 Prometheus/Collector/Tempo 联调、多租户权限与 24 小时稳定性单独验收。

[Grafana OSS 能力](https://grafana.com/docs/grafana/latest/fundamentals/) · [Dashboard](https://grafana.com/docs/grafana/latest/visualizations/dashboards/) · [Explore](https://grafana.com/docs/grafana/latest/visualizations/explore/) · [Alerting](https://grafana.com/docs/grafana/latest/alerting/fundamentals/) · [角色和权限](https://grafana.com/docs/grafana/latest/administration/roles-and-permissions/) · [数据源管理](https://grafana.com/docs/grafana/latest/administration/data-source-management/)
