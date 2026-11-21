# 项目文档分类

**Function3.5**。[版本与测试范围](releases/function3.5.md) · [完整更新文件](releases/function3.5-files.md) · [账号绑定、切换与回退](ops/runtime/nebula-yantu-community.md) · [下一阶段实施顺序](roadmap/next-steps.md)

本轮工程化收口：[全项目实施清单](roadmap/project-review-implementation-2026-11-19.md) · [原生交付与预检](ops/validation/native-delivery.md) · [真实慢 ACK 排查讲义](observability/ack-diagnosis.md)。

按“看现状、查技术、学业务、跑项目、看证据”查找；需要按开发顺序阅读时，使用 [Phase阶段时间线](roadmap/stage-index.md)。阶段正文保留原文件名，改为按主题存放，不再堆在一个目录。

## 先看这几份

|要做什么|入口|
|---|---|
|了解当前功能、实现边界和未通过项|[当前状态](../STATUS.md)、[复现验收](ops/validation/reproduce.md#这次的验证记录)、[Phase49 历史验收](architecture/business/engineering/phase49.md)|
|从空目录构建和启动|[两档复现指南](ops/validation/reproduce.md)|
|日常启动/停止与客户端入口|[运行文档](ops/runtime/README.md)|
|研途客户端与网页同步、故事广场、七日榜、小研与训练授权|[接入改造、启动与验收](ops/runtime/nebula-yantu-community.md)|
|学习小研怎样识别偏好、召回个人要点和组织回答|[个性化记忆进度](reports/xiaoyan-personalization-progress.md)|
|看从通信到业务、模型的完整图|[当前端到端总图](architecture/business/flows/current-end-to-end.md)、[系统总览](architecture/overview/ARCHITECTURE.md)|
|了解后续优先级与验收标准|[ACK 长尾、Envoy 与多机计划](roadmap/next-steps.md)、[近期工程计划](roadmap/engineering-credibility.md)|
|查询监控技术、实测版本与更新方法|[技术应用与版本台账](observability/sources.md)、[真实双实例验收](observability/dual-instance-lab.md)|
|审阅这一轮之后该改什么|[2026-11-19 全项目改进意见（待评审）](roadmap/project-review-2026-11-19.md)|
|实验网关和扩展项目怎么做|[Envoy 隔离实验](observability/envoy-gateway.md)、[C++、DAG 与具身智能评估](roadmap/nebula-cpp-dag-robotics-evaluation.md)|
|学习业务如何接到服务器|[接口与装配讲义](architecture/business/guides/phase49-teaching.md)、[业务教学目录](architecture/business/guides/README.md)|
|找项目文件与实现组件|[项目目录树](architecture/overview/project-layout.md)、[组件索引](architecture/business/indexes/README.md)|

## 按用途分类

|分类|包含内容|
|---|---|
|[系统架构](architecture/README.md)|三端职责、工程目录、底层通信与业务架构|
|[底层通信](architecture/communication/README.md)|Reactor、协程、HTTP、WebSocket、SSE、HTTP/2、TLS、gRPC|
|[业务功能](architecture/business/README.md)|账号聊天、AI/RAG/训练、社区管理、Windows客户端、前端、MQ、工程分层|
|[接口契约](contracts/README.md)|HTTP路由目录、聊天OpenAPI、WebSocket/SSE约定|
|[数据库](database/README.md)|表含义、事务、迁移与Query/DTO/DAO/公开投影分层|
|[运行维护](ops/README.md)|环境、依赖、启动停止、验证回退、排错和历史收口|
|[测试](testing/README.md)|测试入口、学习讲义、工具和笔记|
|[性能](performance/README.md)|测量方法、真实业务基线、容量和未达标记录|
|[可观测性](observability/README.md)|服务目标、采集质量、链路追踪、系统排障课程和扩展遥测|
|[验收报告](reports/README.md)|阶段验收与历史报告|
|[原始证据](evidence/README.md)|受摘要保护的原始记录，以及artifacts中的较新阶段证据|
|[版本对比](releases/README.md)|test/Function版本差异|
|[计划与阶段时间线](roadmap/README.md)|实施进度、阶段索引与扩展学习路线|
|[学习入口](learning/README.md)|通信课程、业务课程、测试讲义和习题|
|[历史归档](archive/README.md)|旧导航、第一阶段资料与原始参考包|

## 阅读与维护规则

1. 正文按主题归档，一篇跨模块阶段文档只保留一份，通过索引从不同主题进入。
2. 历史验收不改成当前通过；Phase49功能回归通过，严格ACK尾延迟门槛未通过。
3. 原始日志和摘要保持字节不变；少数旧入口仅用于兼容证据中的历史链接。
4. 本机账号清单位于被Git忽略的`database/*.local.md`，不上传GitHub；模型、数据库、私钥和私人资料也不属于公开文档。
5. 目录移动记录见 [迁移清单](document-moves.json)，机器可读正文索引见 [catalog.json](catalog.json)。源码和日常部署状态见[状态页](../STATUS.md)。

新增公开页面后，显式运行 `python scripts/project/check_doc_links.py --refresh-catalog` 更新索引；普通校验运行 `python scripts/project/check_doc_links.py`，不会修改文件。目录存在性会检查，页内标题锚点保留原值，不冒充完整Markdown渲染验收。

近期业务装配变更见[社区内容与投递](architecture/business/engineering/community-runtime-modules.md)。上线记录见[变更流水](ops/changelog.md)。
