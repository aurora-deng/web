# 当前状态

**Function3.6 交付**。可检查的 Linux x86-64 ZIP 和离线 Web/Fleet 镜像按发布提交归档。日常网站状态请查下方运行记录。 [发布与对比](docs/releases/function3.6.md) · [部署、校验与回退](docs/ops/runtime/function3.6-deployment.md)

**2026-11-23 交付维护**。业务文档按 17 个模块维护说明和实操，并在 CI 核对全部已登记的 HTTP 业务域。已清理两台虚拟机中的重复发布和实验归档。CentOS 日常站点、网关和 Observer 端口本次均无监听，隔离交付记录见下。 [上线台账](docs/business/release-matrix.md) · [维护记录](docs/ops/runtime/function3.6-artifact-maintenance.md)

**本次隔离回归**。CentOS 业务档 122 项 CTest 通过，零跳过，包含临时 Redis。发布身份、迁移和静态资源联机预检已用真实二进制复验。Windows 源码检查通过，包含 106 项前端测试和 Observer HTTP；研途账号同步测试另行通过。 [测试范围与复验入口](docs/business/release-matrix.md)

**Observer 告警配置**。源码分支修复了托管 Windows 发现的告警配置与诊断日志并发写入冲突，报表查询走只读连接。并发保存和事务边界专项已连续复跑 12 轮。 [故障与复验](docs/observability/validation.md#配置与诊断日志并发写入)

**当前 M00–M07 · 软件路径到达 M08 的 ROS 2 调用边界**。后端 DAG 检查点、技能版本、gRPC 背压、Agent 审批以及合成数据训练和回放均有隔离验收。CentOS 和 Windows 的 Release 各通过 9 项，CentOS 原生 TSan 与 ASan/UBSan 各通过 4 项。完整 gRPC TSan 在现有 Abseil 调用栈报竞态，单列未通过。真实 ONNX 测试命中 13/24，规则仍为默认。 [本轮验收](docs/ops/validation/m00-m07.md) · [Agent 教学](docs/learning/nebula-fleet-agent-and-skills.md) · [LangGraph 实验](docs/learning/langgraph-nebula-lab.md)

**M00–M07 本机车队压测**。4,000 次接收 ACK，零失败。各轮 P99 为 66.54–71.75 ms，超过 50 ms 参考预算。源码验收锚点为 `cpp-systems-m00-m07-lab-20261123`。 [负载及细项](docs/evidence/cpp-systems/20261123/m00-m07.json)

**C++ 系统阶段**。通用 DAG、独立 gRPC 协调器、边缘账本、断线恢复与 Observer 已在 Windows 和 Linux 验证。原生 TSan、ASan/UBSan 通过。CentOS 协调器与 Ubuntu_ros 边缘实现跨虚拟机派发和重启对账，200 次混合接收 0 错误，P99 为 30.59 ms。工作停在具身 Agent 开发前。 [架构](docs/architecture/overview/cpp-systems.md) · [运行与验收](docs/ops/validation/cpp-systems.md) · [跨机实测](docs/ops/validation/cpp-systems-cross-vm.md)

**Function3.5 源码发布**涵盖小研 DAG、长会话记忆、Envoy 映射和 Observer 增量刷新。日常入口和源码分支的版本记录各自可查。[本版验收](docs/releases/function3.5.md) · [逐文件清单](docs/releases/function3.5-files.md) · [后续路线](docs/roadmap/next-steps.md)

小研使用应用层 DAG、长上下文压缩、按账号的偏好记忆和词法召回。Observer 已验证历史快路径、压缩及大盘自动更新。真实模型表现和新版日常环境测量单独验收。[小研进度](docs/reports/xiaoyan-personalization-progress.md) · [Observer 测试](docs/observability/validation.md#查询速度与自动刷新)

网关候选版增加小研的显式资源和业务路由，HTTPS 可使用校验证书的应用上游。DAG 执行器加入并发预算、节点时限、受控重试和标准遥测，具身协作已覆盖合成观测、规划、审核和待确认提案。[网关映射](docs/ops/runtime/nebula-yantu-community.md#小研路由与资源映射) · [工作流与协作](docs/architecture/business/ai/agent-workflow-runtime.md)

**日常网页的历史发布记录**。`18095` 曾启用 Function3.3 的网页和研途账号服务，资源版本 `7f8c7f1b31e4435c`。Windows 原地址已通过 Edge 核对新名称。该次网关使用 `18090` 上的资源版本 `d7a3142ac64b5ae6` 作为业务与身份上游。 [网关启停与回退](docs/ops/runtime/nebula-yantu-community.md#日常发布与回退)

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
