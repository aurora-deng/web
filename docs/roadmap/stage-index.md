# Phase阶段时间线

每阶段只有一份主题正文。本表列至 Phase49。Function3.1 后的[社区拆分](../architecture/business/engineering/community-runtime-modules.md)与[两档复现](../ops/validation/reproduce.md)另有记录。Phase1旧文本见[历史归档](../archive/README.md)，原文档没有独立Phase2页面。

|Phase|正文|大类|
|---|---|---|
|3|[第三阶段：Server Framework Architecture Upgrade](../architecture/communication/reactor/phase3_architecture_upgrade.md)|architecture|
|4|[第四阶段升级文档 · WebSocket 介入与跨 Reactor 出站体系](../architecture/communication/websocket/phase4_upgrade.md)|architecture|
|5|[Phase 5：在 2.0 架构上接入 SSE](../architecture/communication/sse/phase5.md)|architecture|
|6|[Phase 6：HTTP/2 生产接入、教学版与完整流程核对](../architecture/communication/http2/phase6.md)|architecture|
|7|[Phase 7：在现有 HTTP/1.1 与 HTTP/2 服务器中接入 TLS/ALPN](../architecture/communication/tls/phase7.md)|architecture|
|8|[Phase 8：在现有 Web 服务器旁接入 gRPC C++](../architecture/communication/grpc/phase8.md)|architecture|
|9|[Phase 9：从“协议能跑”到“可控、可观测、可恢复”](../architecture/communication/reliability/phase9.md)|architecture|
|10|[Phase 10：HTTP/gRPC 性能路径与架构拆分](../architecture/communication/performance/phase10.md)|architecture|
|11|[Phase 11：账号、社交聊天与 AI 推理平台](../architecture/business/accounts-chat/phase11.md)|architecture|
|12|[Phase 12：持久消息生命周期](../architecture/business/accounts-chat/phase12.md)|architecture|
|13|[Phase 13：群聊治理与权限闭环](../architecture/business/accounts-chat/phase13.md)|architecture|
|14|[Phase 14：联系人、黑名单与多设备在线状态](../architecture/business/accounts-chat/phase14.md)|architecture|
|15|[Phase 15：真实业务性能基线](../architecture/business/runtime/phase15.md)|architecture|
|16|[Phase 16：可观测性、Outbox 主动唤醒与运维闭环](../architecture/business/runtime/phase16.md)|architecture|
|17|[Phase 17：头像、图片与文件消息](../architecture/business/accounts-chat/phase17.md)|architecture|
|18|[Phase 18：AI 生成韧性与资源治理](../architecture/business/ai/phase18.md)|architecture|
|19|[Phase 19：模型变更审计与 AI 生成追踪](../architecture/business/ai/phase19.md)|architecture|
|20|[Phase 20：模型评测集、结果对比与激活质量门禁](../architecture/business/ai/phase20.md)|architecture|
|21|[Phase 21：RAG 知识检索、权限隔离与回答引用链](../architecture/business/ai/phase21.md)|architecture|
|22|[Phase 22：授权数据集与真实 LoRA 微调闭环](../architecture/business/ai/phase22.md)|architecture|
|23|[Phase 23：候选模型真实评测、人工发布与长稳证据](../architecture/business/ai/phase23.md)|architecture|
|24|[Phase 24：训练数据、模型制品与候选版本不可变血缘](../architecture/business/ai/phase24.md)|architecture|
|25|[Phase 25：用户态进程监督与异常恢复](../architecture/business/runtime/phase25.md)|architecture|
|26|[Phase 26：长连接容量、AI 并发与混合负载证据](../architecture/business/runtime/phase26.md)|architecture|
|27|[Phase 27：聊天历史、断线恢复与发送确认闭环](../architecture/business/accounts-chat/phase27.md)|architecture|
|28|[Phase 28：业务执行器、长会话与重连](../architecture/business/runtime/phase28.md)|architecture|
|29|[Phase 29：RocketMQ、Outbox、Inbox 与失败恢复](../architecture/business/messaging/phase29.md)|architecture|
|30|[Phase 30：站点管理与模型控制入口](../architecture/business/community/phase30.md)|architecture|
|31|[Phase 31：苦情树动态、媒体与内容处置](../architecture/business/community/phase31.md)|architecture|
|32|[Phase 32：三维苦情树与有界浏览](../architecture/business/community/phase32.md)|architecture|
|33|[Phase 33：七日热度、全服公告和本地语音](../architecture/business/community/phase33.md)|architecture|
|34|[Phase 34：Function3 验收记录](../reports/stages/phase34.md)|reports|
|35|[Phase35：复用原业务库、语音修复、Redis 与界面重构](../architecture/business/client-ui/phase35.md)|architecture|
|36|[Phase36：聊天布局与后端边界](../architecture/business/client-ui/phase36.md)|architecture|
|37|[Phase37：统一动态、个人主页与精致神树](../architecture/business/community/phase37.md)|architecture|
|38|[Phase38：Windows 本机入口与私人知识空间](../architecture/business/client-ui/phase38.md)|architecture|
|39|[Phase39：三种AI入口、私人推理、确认行动与旧资料迁移](../architecture/business/ai/phase39.md)|architecture|
|40|[Phase40：语音表达、个人设置与免费云适配](../architecture/business/ai/phase40.md)|architecture|
|41|[Phase41：完整回归、真实链路与性能验收](../reports/stages/phase41.md)|reports|
|42|[Phase42：保持架构方向，优化响应与神树外观](../architecture/business/community/phase42.md)|architecture|
|43|[Phase43：可恢复基线与验收分级](../architecture/business/engineering/phase43.md)|architecture|
|44|[Phase44：可执行的工程检查](../architecture/business/engineering/phase44.md)|architecture|
|45|[Phase45：聊天领域边界、批量读取与社交接口](../architecture/business/accounts-chat/phase45.md)|architecture|
|46|[Phase46：前端单一上下文与真实业务交互](../architecture/business/client-ui/phase46.md)|architecture|
|47|[Phase47：综合验收、性能结果与交付限制](../reports/stages/phase47.md)|reports|
|48|[Phase48：补齐配色、领域拆分与 Outbox 提交优化](../architecture/business/engineering/phase48.md)|architecture|
|49|[Phase49：业务契约对齐、领域装配与数据库对象分层](../architecture/business/engineering/phase49.md)|architecture|

Phase50–53目前仅是 [后续建议](next-steps.md)，不计入上表已形成实现记录的阶段。

[按主题查找](../README.md) · [综合报告](../reports/README.md) · [整体图](../architecture/business/flows/README.md)
