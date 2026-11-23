# NEBULA · C++20 Web Server / Function3.6

工程化改造：[原生包复现/预检](docs/ops/validation/native-delivery.md)、[真实 Prometheus 与慢 ACK 排查](docs/observability/ack-diagnosis.md)、[R1–R12 实施和验收边界](docs/roadmap/project-review-implementation-2026-11-19.md)。保持通信核心与原业务库，新增依赖仍单项授权。

NEBULA 用 Linux C++20 服务器运行缘续-聊天、缘生-广场、研途和小研，并用 Observer 观测这些业务的真实流量。

**Function3.6** 增加后端 C++ DAG 检查点、独立 gRPC 车队、Agent 提案审批、技能版本和 Observer 回放。发布分为离线 Web/Fleet Docker 镜像、可运行的 Linux ZIP 和源码 ZIP。 [发布说明与 Function3.5 对比](docs/releases/function3.6.md) · [部署、校验与回退](docs/ops/runtime/function3.6-deployment.md) · [下载](https://github.com/aurora-deng/web/releases/tag/Function3.6)

[各业务说明与学习](docs/business/README.md) · [上线台账](docs/business/release-matrix.md) · [参与维护](CONTRIBUTING.md)

[文档分类](docs/README.md) · [当前架构](docs/architecture/overview/ARCHITECTURE.md) · [完整调用图](docs/architecture/business/flows/current-end-to-end.md) · [当前状态](STATUS.md) · [后续计划](docs/roadmap/next-steps.md)

[项目文件架构](docs/architecture/overview/project-layout.md) · [从干净目录复现](docs/ops/validation/reproduce.md) · [Obsidian 学习计划与助教提示词](docs/learning/nebula-performance-and-reliability.md)

[Observer 监控与系统排障课程](docs/observability/README.md)

[跨业务埋点与真实验证](docs/observability/business-instrumentation.md) · [全项目下一阶段改进意见（待评审）](docs/roadmap/project-review-2026-11-19.md)

[结构化日志、七天清理与查询编辑器](docs/observability/logging.md)说明业务报错如何进入日志工作台、关联调用链，以及函数耗时的实际采集条件。

## 当前功能与边界

|区域|已有能力|需要区分的边界|
|---|---|---|
|账号与社交|注册/登录、Session/CSRF、好友、备注/免打扰/黑名单、在线状态|权限由服务器校验，隐藏按钮不代表授权|
|私聊与群聊|离线发送、持久化ACK、未读/送达/已读、撤回、群治理、历史/搜索定位、附件|ACK不是对方已读；重试保留原幂等身份|
|缘生-广场与个人动态|神树、故事广场、分类七日榜、点赞回复、举报隐藏与权限化主页|公开故事与个人动态分别检查可见范围|
|研途|考研计划、笔记、计时、资料、练习，使用 NEBULA 账号同步|下载目录、设备凭据和旧独立研途资料按设备管理|
|小研与群 AI|好友式小研、全局侧栏、群 AI；可用模型取决于入口和配置|Web 页可用站点模型、本机 Ollama 或自备 API，Windows 私人检索需本机入口|
|RAG与模型治理|授权词法检索/引用、记忆、数据集/制品版本、评测门禁、训练转换及人工发布回滚工具|训练流水线验证不等于模型质量提高；不自动训练发布所有模型|
|语音与运行保障|独立TTS、公告与试听/回退、指标/健康、持久事件、Redis/MQ可选集成|真实云Key、主观自然度和正式容量有未验收项|

功能、替身、真实服务和性能分别记录。复现结果见[当前验收](docs/ops/validation/reproduce.md#这次的验证记录)，性能基线见 [Phase49 原始证据](artifacts/function3/phase49/README.md)。

## 架构分工

```text
浏览器 frontend/phase11
  → 可直接访问Linux，或经Windows本机入口
  → 通信核心 backend/server（Reactor、HTTP/WS/SSE/H2/TLS）
  → 共享业务 backend/application（Service、Port、事务、DAO、Outbox）
  → AI核心 backend/ai（Provider、RAG、模型、独立重任务Worker）

Windows client
  → 本机代理、私人历史/记忆/词法索引、确认操作、凭据保护
  → 只把必要且获授权的片段与上下文发给推理服务

研途网关 /api/study/
  → backend/study 核对 NEBULA 会话，读取该账号的学习工作区

Observer
  → Prometheus 指标、Tempo 调用链和结构化日志的排查工作区

独立 C++ 车队
  → backend/agent 读取工单与提案，backend/workflow 执行与检查点
  → backend/fleet 协调器通过 gRPC 向边缘下发，SQLite 保存工单与回执
  → software edge / NavigationSkill，ROS 2 仿真独立验证
```

服务采用模块化单体，推理、语音与显式训练任务各有独立进程边界。社区内容装配负责神树、动态、举报和媒体，社区投递装配负责 Outbox、Broker、重放和通知。[目录与依赖规则](docs/architecture/overview/project-layout.md) · [社区模块说明](docs/architecture/business/engineering/community-runtime-modules.md)

## 数据、接口与隐私

- Linux 复用既有 PostgreSQL 业务库。当前完整业务迁移到 026，两档复现文档另保留早期 024 的实测记录。
- 研途使用同一账号身份，首次打开时建立对应学习工作区。Windows 原独立研途资料保留在设备上。[研途运行手册](docs/ops/runtime/nebula-yantu-community.md)
- 消息与Outbox同任务、同连接事务提交，之后分别形成持久化ACK和实时通知。
- C++ 路由目录列出 124 项 HTTP 方法与路径及 18 个领域。聊天字段、WS/SSE、研途和 Windows 本地接口分别记录契约。
- Windows私人资料/索引在本机，授权片段可能上传推理；DPAPI保护凭据，未宣称正文整库加密或全部离线。
- 网页助手要求非保密上传提示；群AI不能读取个人私人知识。Key、模型、数据库、私人资料和本机账号文件不提交Git。

详见 [数据库分层](docs/database/domain-layering.md)、[接口契约](docs/contracts/README.md)、[接入讲义](docs/architecture/business/guides/phase49-teaching.md)。

## 启动、构建与检查入口

|需要做什么|入口|
|---|---|
|从 Release 安装 Docker 或 Linux ZIP|[Function3.6 交付、校验与部署](docs/ops/runtime/function3.6-deployment.md)|
|Linux日常启动/停止，原账号库与端口|[运行说明](docs/ops/runtime/function3-runtime.md)|
|Linux 从源码复现通信档、业务档|[复现命令和验证结果](docs/ops/validation/reproduce.md)|
|Windows 本机入口、研途 TLS 与账号切换|[客户端构建](docs/ops/runtime/phase36-41-local-preview.md) · [研途连接](docs/ops/runtime/nebula-yantu-community.md#function33-首次绑定与切号)|
|已有环境、依赖位置与审批|[依赖目录](docs/ops/dependencies/README.md)|
|测试矩阵、源码检查与故障验证|[测试入口](docs/testing/README.md)|
|部署前核对、回退与证据口径|[验证与回退](docs/ops/validation/phase49-validation-and-rollback.md)|

日常实例使用 18090/18443，既有隔离预览使用 18094/18444。新的复现脚本选择临时 HTTP 端口，业务档使用独立的临时 PostgreSQL。[运行状态](STATUS.md)

文档检查可运行：

```text
python scripts/project/check_doc_links.py
python scripts/project/ci_checks.py source
```

新增公开文档后使用 `check_doc_links.py --refresh-catalog` 显式刷新目录；历史证据、私有账号文件和冻结核心不由文档改链工具重写。

## 验收状态

M00–M07 的 CentOS 和 Windows 独立 Release 各通过 9 项，CentOS 原生 TSan 与 ASan/UBSan 各通过 4 项，完整 gRPC TSan 仍有依赖线程告警。车队固定负载 4,000 次接收 ACK 无失败，P99 为 66.54–71.75 ms，超过 50 ms 参考预算。 [本轮软件验收](docs/ops/validation/m00-m07.md)

复现分支的两个 Linux Release 档位均从空构建目录完成构建与启动。业务档 112 项 CTest 通过，跳过 1 项可选 Redis 契约。注册登录、WebSocket、果实幂等发布、Outbox/SSE 和重启恢复均通过。干净 Git 检出通过 79 项前端测试和 23 项 Python 工程检查。[复现记录](docs/ops/validation/reproduce.md#这次的验证记录)

Phase49 于 2026-10-31 归档，其 Release、Debug 和 ASan/LSan/UBSan 各 115 项及其他验证属于当时的配置。[Phase49 验收](docs/architecture/business/engineering/phase49.md)

**严格性能门槛仍未通过。** Phase49三轮/fast中位约-3.89%，混合吞吐中位约-16.93%；第三轮聊天ACK P99 8.710→15.545ms（+78.47%），业务错误0。原始不利结果与中断记录保留，不宣称生产就绪。

## 建议的下一步

1. **ACK 长尾**。复用已归档的 A/A 与逐消息配对，固定负载复查排队和提交，按三组对照验收收益。
2. **Envoy 网关**。先实测单上游协议、入口故障和额外开销，再演练双上游健康摘除、连接排空和恢复。
3. **可靠性与真实接入**。完成受限长稳和跨主机网络故障，按需验收 C++ Wasm、真实 DAG/Agent 或设备生产者，再推进有独立证据的 AI 治理和受控试运行。

目标、测量口径和过关标准见[后续路线图](docs/roadmap/next-steps.md)与[技术应用和版本台账](docs/observability/sources.md)。

## 学习与历史

- [按主题阅读](docs/README.md)、[Phase3–49时间线](docs/roadmap/stage-index.md)、[业务教学](docs/architecture/business/guides/README.md)。
- [组件索引](docs/architecture/business/indexes/README.md)、[历史通信详细讲义](docs/architecture/communication/runtime-baseline.md)。
- [版本差异](docs/releases/README.md)、[原始验收证据](docs/evidence/README.md)。较早README和当时未推送/未部署的叙述可在Git历史检查点`5f18761`查看。
