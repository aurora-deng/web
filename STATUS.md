# 当前状态

**Function3.3 开发快照**汇集 Observer、研途和小研。缘续-聊天在会话检查完成后才呈现登录，缘生-广场增加滚轮换页过渡，研途按当前 NEBULA 账号打开工作区。日常入口的部署版本见下方。[本版验收](docs/releases/function3.3.md) · [后续路线](docs/roadmap/next-steps.md)

**当前日常网页**。`18095` 已启用 Function3.3 的网页和研途账号服务，资源版本 `7f8c7f1b31e4435c`。Windows 原地址已通过 Edge 核对新名称。直连 `18090` 继续提供上一轮资源版本 `d7a3142ac64b5ae6`，仍是网关的业务与身份上游。[当前网关与回退](docs/ops/runtime/nebula-yantu-community.md#日常发布与回退)

Linux 双实例隔离验收通过。25 条消息跨实例送达，A 在最后一次持久化 ACK 后强杀，B 仍可接收、查历史并按原键去重。Prometheus 双实例查询、Tempo 链路回读、Collector 中断恢复和本机 Alertmanager 通知均通过实测。[双实例验收](docs/observability/validation.md#双实例实时投递与告警验收)

上一轮 `d7a3142ac64b5ae6` 部署了故事广场、七日心动榜与统一小研。真实 PostgreSQL、社区 HTTP 和 Envoy 隔离测试通过，原业务库已记录 5 篇示例和 12 条评论。87 项前端测试与 Edge 三尺寸旅程通过，模型使用合成回应验证。Observer 已采集业务进程。[上一轮发布记录](docs/ops/runtime/nebula-yantu-community.md#上一轮社区发布)

2026-11-21，Windows 转发的新版 `18095` 与日常 `18090` 已共用原业务库 `phase11`。原库 68 个账号的登录凭据已逐个核对，隔离库中经确认的 3 条树下回复已迁入本人可见的个人动态。Observer `18791` 已采到切换后的 NEBULA 进程数据。[账号合并与验收](docs/ops/runtime/nebula-yantu-community.md#账号与数据归属)

切换前，Envoy 1.40.2 在隔离站通过研途登录、同账号会话同步、附件读取和越权拦截。分类榜和训练授权完成真实 HTTP 验证。单上游 TLS、长连接、双上游摘除与 Wasm 仍各有独立的验收范围。[研途发布记录](docs/ops/runtime/nebula-yantu-community.md#本次结果) · [网关手册](docs/observability/envoy-gateway.md)

11 月 19 日工程收口包含 AI 准备/终态/审计拆分、PG 工作单元、Observer 查询服务、原生 Worker 交付预检、有界 Prometheus 批量查询及七天临时遥测清理。端口 18798 用于单独的慢 ACK 隔离实验，复验按手册另开运行目录。[实施清单](docs/roadmap/project-review-implementation-2026-11-19.md) · [真实慢 ACK 教学](docs/observability/ack-diagnosis.md) · [原生交付](docs/ops/validation/native-delivery.md)

- **开发基线**。`Function3.2` 包含社区模块拆分、Linux 复现、连接池恢复和 ACK 分段采样。`Function3.3` 在这个基线之上扩展研途、Observer 与入口体验。[项目目录](docs/architecture/overview/project-layout.md) · [历史基线](docs/releases/function3.2.md)
- **复现已验证**：通信档与业务档均从空目录完成 Release 构建和启动。业务档使用临时 PostgreSQL 应用 001–024，112 项 CTest 通过，1 项可选 Redis 契约跳过。注册登录、WebSocket、幂等发布、SSE 与重启恢复通过。干净 Git 检出通过 79 项前端测试和 23 项 Python 工程检查。[复现记录](docs/ops/validation/reproduce.md)
- **日常业务服务**已通过 Release 构建，原库启动核验迁移 001 至 026。研途账号工作区经 `18095` 使用。业务日志和进程采样由 Observer 采集。[日常启动](docs/ops/runtime/function3-runtime.md) · [启用与回退记录](docs/observability/daily-rollout.md)
- **隔离定位**：数据库会话被终止和 ACK 后进程被强杀的恢复演练通过。16 工作端压测尾部显著增加，下一步拆查业务执行器排队及事务提交前路径。[测量结果](docs/releases/function3.2.md#性能定位当前证据指向哪里)
- **下一步**：先固定同配置的 ACK 基线，再验收 Envoy 单上游、同机双上游、24 小时长稳和跨主机故障。隔离十对 A/A 已完成、噪声偏高，Phase49 的 ACK P99 历史超标结论保留。容器复现和远端业务 runner 各有独立验收项。[实施顺序和门槛](docs/roadmap/next-steps.md) · [技术版本与接入](docs/observability/sources.md)

[变更流水](docs/ops/changelog.md) · [文档导航](docs/README.md) · [下一阶段](docs/roadmap/next-steps.md)

11 月 19 日回归中，原生 Release 121 项通过、1 项 Redis 跳过，Debug+UBSan 118 项通过、1 项 Redis 跳过。通信档从空目录构建、原生包解包启动、真实 Prometheus/Node Exporter、慢 ACK/强杀恢复和三组监控浏览器旅程通过。完整模型质量、远端 CI、多机后端与长稳继续单列验收。[选定证据](docs/evidence/function3/project-review-20261119/summary.json)

## Observer 研发诊断

- `feature/observability` 的日常监控入口为 18791，采集原 18090 聊天与苦情树服务。18793 对应隔离埋点版，提供函数分位数和调用链。[启动与当前入口](docs/observability/setup.md) · [文档首页](docs/observability/README.md)
- 服务目标页基于轻量档的真实请求增量，显示事件量、采集质量、六小时预算和双窗口状态，支持按 HTTP 5xx 下钻。[架构与取数口径](docs/observability/architecture.md#服务目标与采集质量) · [验证记录](docs/observability/validation.md)
- 业务 HTTP 追踪采样已与错误日志解耦。六轮同机对照和逐条 ACK 分段已归档。CI 增加双平台 Observer 契约，Linux 交付路径已验证 114 项 CTest 和解包启动。[采样对照与交付证据](docs/observability/validation.md#2026-11-16-http-采样开销与-ack-长尾) · [CI 与交付](docs/ops/validation/reproduce.md#ci-与交付)
- 逐条 ACK 可按数据库进程和提交区间关联 PostgreSQL 等待，附同窗磁盘采样。新消息的序号分配和插入合并为一次数据库调用，三对隔离压测已归档。[提交长尾与写入对照](docs/observability/validation.md#2026-11-16-postgresql-提交长尾与消息写入)
- 内置实时面板原图更新，保留图例选择、焦点和游标；查询失败时清除旧曲线，恢复后重新显示。遥测最多保留七天，可导出 PNG 和 CSV。[刷新与恢复验证](docs/observability/validation.md#2026-11-18-实时刷新与恢复)
- 指标先显示，健康、告警、版本与标注独立更新；慢附加查询跨指标刷新合并，失败可单独重试。真实 18090 指标和受控延迟的浏览器验证通过。[查询隔离验证](docs/observability/validation.md#2026-11-19-附加查询隔离与首图显示)
- 空白指标按实际采样区分缺指标、无新增调用和采集失败，正常零值照常显示。浏览器读取超时后给出重试建议。[诊断规则](docs/observability/architecture.md#空白指标与查询失败)
- Observer 增加多查询编辑器及结构化日志工作台，支持日志与 Trace 双向关联。18791 已采集日常 18090 的真实函数直方图、日志与数据库调用链；无调用的函数保留空态。日志索引和自有轮转文件均执行七天保留。[组件与接入](docs/observability/logging.md)
- 本轮将函数目录扩展为 26 项，覆盖账号、神树、数据库、AI/RAG 等；异步生成的完成、取消与超时只计一次，长任务增加至 300 秒耗时桶。先使用独立 18797 验证，日常程序保持上一版；详见[埋点与边界](docs/observability/business-instrumentation.md)及[本轮验收](docs/observability/validation.md#2026-11-19-跨业务埋点)。本轮修改按[全项目实施清单](docs/roadmap/project-review-implementation-2026-11-19.md)验收。
- Prometheus、Node Exporter、Collector、Tempo 和本机 Alertmanager 已通过隔离联调。生产 Loki、持续剖析、多机访问治理与对外通知仍待验收。DAG、Agent、设备仍有标明的模拟生产者。[差距与演进顺序](docs/observability/sources.md)
- 学习按健康、四个黄金信号、服务目标、调用链、故障演练和新来源接入展开。[六课实操与提示词](docs/observability/learning.md)
