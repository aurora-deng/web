# Function3 实施与验收台账

Function3.6 交付后端 C++ DAG、gRPC 车队、Agent 提案审批与 Skill 合同。GitHub Release 可下载 Linux ZIP 和离线 Web/Fleet 镜像。各业务附有说明、实操与上线复验条件。 [发布记录](../releases/function3.6.md) · [业务学习入口](../business/README.md) · [上线台账](../business/release-matrix.md)

Function3.3 整理了缘续-聊天、缘生-广场命名与切页，新增研途首次账号绑定和 Windows 返回原模块的切号流程。监控、接口、目录与运行证据继续按原主题维护。[版本说明](../releases/function3.3.md) · [目录](../architecture/overview/project-layout.md)

Function3.5 汇集小研个性化记忆与 DAG、Envoy 映射及 Observer 查询优化。小研已验证账号隔离、跨页面恢复和模型调用载荷，模型准确率另列验收。Observer 隔离验收覆盖历史读取、压缩传输和自动刷新节点复用。[Function3.5 发布说明](../releases/function3.5.md) · [小研进度](../reports/xiaoyan-personalization-progress.md) · [监控测试](../observability/validation.md#查询速度与自动刷新)

编号阶段的最近验收为 [Phase49](../architecture/business/engineering/phase49.md)。在 Function3.1 之后，本地分支又完成了[社区装配拆分](../architecture/business/engineering/community-runtime-modules.md)与[两档复现](../ops/validation/reproduce.md)。Phase49 的 ACK 尾延迟门槛仍未通过。

Function3 初始基线为 Function2.1 `ffc0e14`，早期开发分支为 `feature/function3-tree`。[Function3.2 复现与性能基线](../releases/function3.2.md)

研发诊断平台从 Function3.2 独立开发。阶段交付为[可观测性工作区](../observability/README.md)，逐项验收与未覆盖事项见[验证记录](../observability/validation.md)。

Envoy 1.40.2 已在 `18095` 隔离站完成研途 HTTP 代理实测。账号会话、笔记同步、附件和访问权限已验证。网关的双上游、其他协议和 C++ Wasm 继续单独验收。[研途发布记录](../ops/runtime/nebula-yantu-community.md#本次结果) · [启动与观察](../observability/envoy-gateway.md) · [C++、DAG 和机器人接入方向](nebula-cpp-dag-robotics-evaluation.md)

11 月 21 日，在两库备份和恢复副本演练通过后，新版研途入口切换到原 `phase11`。两个业务入口通过迁移 026 和就绪检查，68 个原账号保留，3 条回复进入本人可见的个人动态。Observer 保留历史并继续采集新的业务进程。[合并与验收](../ops/runtime/nebula-yantu-community.md#账号与数据归属)

同日完成故事广场的关键词与标签浏览、楼主回复查询和七日榜卡片。全站以小研呈现原个人 AI 好友与侧栏助手，网页可切换本站模型、本机 Ollama 和自带 API。通过真实 PostgreSQL 和 Envoy 隔离验收后，现有两入口已同步，原库新增 5 篇考研示例和 12 条评论，Observer 已采集到新业务进程。[过程与结果](../ops/runtime/nebula-yantu-community.md#日常发布与回退)

11 月 20 日，同一虚拟机的直连双实例隔离验收通过。迁移 025 支撑实时转发，覆盖 25 条跨实例投递、已确认消息的历史恢复和原键去重。真实 Prometheus、Collector/Tempo 以及本机 Alertmanager 完成短时演练。接下来的主要门槛是 ACK 长尾、Envoy 单上游与双上游、异机故障和长稳。[双实例验收](../observability/validation.md#双实例实时投递与告警验收) · [下一阶段计划](next-steps.md)

监控大盘补齐共享游标、区间选择、面板检查、CSV、DAG 节点联动和 Agent 分类统计。新增[轻量实时档](../observability/setup.md#轻量实时档)，以隔离实例验证真实数据到查询接口。

新增[可配置数据源与分布式排障工作区](../observability/workspace.md)，覆盖自定义仪表盘和模板、资源标签、Tempo、Loki 与真实 NEBULA 链路排查，接口和浏览器实际验收记入[验证记录](../observability/validation.md)。

新增[主机、服务、进程、线程与函数观测](../observability/resources.md)，使用真实 /proc、业务直方图以及隔离绑核、syscall、指令、堆实验。Pyroscope 与 Jaeger v3 已通过协议契约，真实后端和多机现场验收仍待完成。

补充[Explore 实际延迟桶与 P99、BC/APP/INSTANCE、两条告警、Span 调用树、Linux 函数与配置即代码](../observability/explore-and-connect.md)。DAG、Agent 和设备按来源回读已保存的同一批事件，验证接收与去重。

补充「服务目标与采集质量」。将原始计数采样、SLO 计算、HTTP 查询和页面显示分层，核对受影响的请求量、采集覆盖和双窗口预算，支持同窗 HTTP 5xx Trace。按[验证记录](../observability/validation.md#服务目标与采集质量的真实验证)查看实际范围。下一步优先统一跨实例身份并接入真实日志，详细理由见[演进顺序](../observability/sources.md)。

2026-11-18 补充内置总览与业务面板的原图刷新，保留图例、焦点与悬停游标，验证查询失败后的曲线清除与恢复。18791 用于日常 18090 监控，18793 用于隔离埋点版。七天留存、批量查询、页面探针和导图操作见[当前启动说明](../observability/setup.md)，浏览器与接口证据见[实时刷新验收](../observability/validation.md#2026-11-18-实时刷新与恢复)。

同日继续完善空白指标诊断，按实际采样区分缺少耗时指标、无新增业务调用、采集失败、过期和预热。查询增加八秒超时并保留失败原因，说明与组件入口见[空白指标与查询失败](../observability/architecture.md#空白指标与查询失败)。

2026-11-19 收尾[结构化日志与查询编辑器](../observability/logging.md)。将日志格式、异步队列、文件轮转、存储、采集与页面分开，接通业务 HTTP/WS 错误和 Observer 报错。日志索引滚动保留七天，自有文本按日轮转并限额。函数页明确显示监控目标和指标族。经用户批准，完整功能配置版本已启用到日常 18090，原库、68 个账号、页面、证书及通信核心不变；18791 已采到真实函数耗时、业务日志和数据库调用链，详见[日常验收](../observability/daily-rollout.md)。修复隔离启动配置的原子发布与旧端口读取时序。后续跨实例身份和外部 Loki 的实机部署仍单列验收。

2026-11-19 继续完成[大盘附加查询隔离](../observability/architecture.md#页面查询与生命周期)。指标先绘制，告警等附加请求由独立模块管理；五秒刷新合并未完成请求，切页取消并忽略迟到结果，支持失败项目单独重试。新增四项生命周期契约及真实指标浏览器验收，通信核心、原业务库和日常业务进程不变。[实测和边界](../observability/validation.md#2026-11-19-附加查询隔离与首图显示)

同日新增[跨业务埋点](../observability/business-instrumentation.md)：固定目录 26 项，完成次数、执行中数量、墙钟分布分开；异步生成计到终态，AI 准备/分派跨线程传播 Trace。社区 SQL 分段与真实探针链路已接通，函数页按业务域筛选。测试与性能结论按[本轮验收](../observability/validation.md#2026-11-19-跨业务埋点)记录，新业务程序先独立验证。全项目后续改进列为[待评审意见](project-review-2026-11-19.md)，不自动实施其新范围。

## 现场核对（2026-10-23）

- 本地最初工作区干净。
- Linux `/home/pikachu/phase27-chat-reliability` 的聊天前端、历史模块、HTTP Session 和聊天 HTTP 适配器与基线逐文件 SHA-256 一致。
- `/home/pikachu/phase11-full-src` 的运行副本较旧，本轮不覆盖。
- Linux 约 7.5 GiB 内存、5.9 GiB 可用内存、7.4 GiB 剩余磁盘；已有 Java 24.0.2、RocketMQ 5.1.4、PostgreSQL、libsodium、curl、ONNX 环境。
- Three.js、C++ SDK、sherpa-onnx、Kokoro 与 FFmpeg 已逐项获准，程序均位于指定路径。复用现有 RocketMQ 5.1.4，没有下载 5.5.1。

## 阶段状态

| 阶段 | 状态 | 验证 |
|---|---|---|
| Phase28 模块/异步入口/前端/重连 | 功能验证通过 | Node 18/18，真实旧聊天黑盒通过 |
| Phase29 RocketMQ 与 Outbox | 真实链路与故障验证通过 | 5.1.4 FIFO 顺序通过，NORMAL TTS 真实消费通过 |
| Phase30 后台与权限 | 功能验证通过 | 越权、最后管理员、会话吊销、异步模型启用和失败状态均已验证 |
| Phase31 树业务与媒体 | 真实闭环通过 | PNG/WAV、坏媒体/超长语音拒绝、隐藏旧链接 404 |
| Phase32 3D 神树 | 分区与容量抽样通过 | Edge WebGL、移动、24 枚分区通过 |
| Phase33 榜单与语音公告 | 真实闭环通过 | 两种真实音频、公告播放领取去重通过 |
| Phase34 综合验收 | 存在未达标项 | Release/Debug/Sanitizer均107项通过；I/O对照已记录，ACK P99未达标 |
| Phase35 原库/语音/Redis/界面 | 本轮功能与回归通过 | Release/Debug/Sanitizer各108项；Node18项；原68账号保留；真实音频、移动三页、Redis回源通过；分类读中位吞吐+29.39%，不覆盖旧ACK门槛 |

Phase28–34历史验收见 [Phase34](../reports/stages/phase34.md)；当前增量见 [Phase35](../architecture/business/client-ui/phase35.md)。上述“已实现”不等于生产认证。整体图见 [端到端架构](../architecture/business/flows/function3-end-to-end.md)，组件索引见 [代码索引](../architecture/business/indexes/function3-code-index.md)。

## 文件与依赖规则

业务模块自行拥有领域、端口、应用服务与适配器；接口不暴露协议和数据库驱动类型。一个组件一个同名目录，配对源文件一起保存，纯接口不创建空实现文件。新增文件索引、调用图和测试证据随阶段同步。

业务数据与 Outbox 必须在同一事务提交，MQ 不替代客户端 ACK；媒体不扩大原聊天附件权限，树内容不自动进入训练数据。

## Phase35 建立的日常使用入口

- 聊天：`/phase11/`；社区：`/phase11/tree/`；后台：`/phase11/admin/`。
- 三者复用账号 Cookie；普通用户不因成为群主而获得站点管理员权限。
- 首个管理员通过 `function3-site-admin` 显式指定现有普通账号；不会创建默认管理员密码。
- 默认公告声音关闭，用户主动开启并选择音色；播放声音不需要开启麦克风，只有发布录音时才申请录音权限。
- 部署先按顺序执行新增迁移，再配置已安装依赖；应用不会自动迁移或下载。
- 日常使用 `scripts/function3/runtime/service.py`，复用原 `phase11 / 5432`、原密钥和文件根目录，HTTP18090。升级前备份；本轮68个原账号摘要不变。首个管理员已按授权选择已有 `nebula_owner`，密码不变。
- `scripts/function3/validation/run_server.py` 只用于既有测试库与18094端口，不作为日常入口。本轮没有创建新数据库。
- 现有Redis6379用于公共分类/榜单读缓存；默认明确的本地持久投递支持自动公告语音。实际使用RocketMQ时必须显式配置可达Proxy。
- 统一顶部/移动底部导航，树/广场/榜单分视图，真实回复目标与举报弹层。操作见[日常启动说明](../ops/runtime/function3-runtime.md)。

功能测试与性能门槛分别记录，性能失败不会被功能通过覆盖。

学习讲义：[一枚果实如何穿过服务器](../architecture/business/guides/function3-walkthrough.md)。本轮没有把性能未达标标为通过。

## Phase36–41 后续实施（2026-10-25）

- Phase36：聊天固定Grid区域、56+272/280+消息区、暗/浅主题、批量会话摘要与新backend边界已构建回归。
- Phase37：109项Release回归通过；统一动态/主页/收藏/可见性/管理删除和旧媒体权限真实HTTP闭环通过，神树与主页Edge检查通过。
- Phase38：Boost1.92已获准并核验，Windows系统SQLite3.51.1与DPAPI真实检查通过；两个助手入口的分离历史、共享记忆、中文词法检索和授权过滤通过。原生TLS/HTTP/WS/SSE代理、TXT/MD/PDF/DOCX解析、真实切号隔离与浏览器联调通过。
- Phase39：好友式/全局私人助手、群AI显式/提及/回复触发、限时私人推理、确认式白名单操作、旧资料复制校验与主动清除已实现；真实ONNX与迁移闭环通过。当前模型工具调用明确降级为问答/草稿，不声称自主Agent。
- Phase40：个人DPAPI语音设置、站点音色审计、Azure/Gemini固定适配、本地断句和语速已实现；Release111项、Windows5项契约及真实缺Key回退通过。真实云调用缺Key未验收，听感仍需人工判断。
- Phase41：Release/Debug/Sanitizer各111项、Windows两种构建各5项、Node18项及真实代理/生成/取消/Worker故障恢复通过。三轮 /fast中位回退6.56%合格，但两轮持久化ACK P99超标，性能门禁未通过；原始结果与WAL诊断见[Phase41](../reports/stages/phase41.md)。本轮不切换日常服务、不推送远程。
- 18090仍运行此前版本；原业务库/账号未改。开发验证使用已有隔离库与18094/18444，不创建新的业务数据库。
- Phase34原持久化ACK P99未达标记录保留。

## Phase42：响应与外观优化（架构方向保持不变）

普通短请求复用已验证的TLS连接，神树批量读取与前十查询合并，静态树干合批、离屏暂停与果实资源增量更新；花穗、珠光果实、根部落花与窄屏构图细化。既有客户端/服务端/前端职责与数据库不变；测试与完整文件索引见[Phase42](../architecture/business/community/phase42.md)。


## 2026-11-19 全项目改进意见实施

- 本轮按 [R1–R12 实施表](project-review-implementation-2026-11-19.md)推进，保留此前未提交成果和原通信核心。
- AI 编排拆分准备、终态、审计和共享状态；PG 同连接事务独立为工作单元；Observer 指标/Trace/实验查询与 HTTP 路由分离。
- 新增原生交付清单、Worker/外部库预检、独立模块链接测试和显式 CI 原生档；CI 临时产物统一七天。
- 已获批安装 Prometheus/Node Exporter，并建立真实 `metrics` 隔离运行档。真实行锁慢 ACK 与幂等恢复讲义见 [诊断课程](../observability/ack-diagnosis.md)。
- 具体测试、浏览器、资源和待验收项见 [本轮验证](../observability/validation.md#2026-11-19-全项目改进意见落地)；不把本地成功称作远端 CI、真实模型质量或全平台生产治理完成。

- 本轮收尾：原生 Release 121 通过/1 Redis 跳过，Debug+UBSan 118 通过/1 Redis 跳过；通信档空目录构建、原生交付解包、真实后端故障恢复及三组监控浏览器旅程通过。首次采集基线、真实 PromQL 和多实例过滤问题已修复。完整范围和缺证据事项均保留在验收记录，不宣称全量企业平台已验收。
