# Function3 代码组件索引

路径相对项目根目录。每个组件保持同名目录，头文件与实现文件放在一起；纯端口不创建空实现。

| 模块 | 层 | 组件 | 文件 |
|---|---|---|---|
| admin | application | `AdminService` | [AdminService.cpp](../../../backend/application/modules/admin/application/AdminService/AdminService.cpp), [AdminService.h](../../../backend/application/modules/admin/application/AdminService/AdminService.h) |
| admin | application | `ModelAdminService` | [ModelAdminService.cpp](../../../backend/application/modules/admin/application/ModelAdminService/ModelAdminService.cpp), [ModelAdminService.h](../../../backend/application/modules/admin/application/ModelAdminService/ModelAdminService.h) |
| admin | infrastructure | `AdminPostgresRepository` | [AdminPostgresRepository.cpp](../../../backend/application/modules/admin/infrastructure/AdminPostgresRepository/AdminPostgresRepository.cpp), [AdminPostgresRepository.h](../../../backend/application/modules/admin/infrastructure/AdminPostgresRepository/AdminPostgresRepository.h) |
| admin | infrastructure | `Phase11ModelAdministration` | [Phase11ModelAdministration.cpp](../../../backend/application/modules/admin/infrastructure/Phase11ModelAdministration/Phase11ModelAdministration.cpp), [Phase11ModelAdministration.h](../../../backend/application/modules/admin/infrastructure/Phase11ModelAdministration/Phase11ModelAdministration.h) |
| admin | ports | `IAdminRepository` | [IAdminRepository.h](../../../backend/application/modules/admin/ports/IAdminRepository/IAdminRepository.h) |
| admin | ports | `IModelAdministration` | [IModelAdministration.h](../../../backend/application/modules/admin/ports/IModelAdministration/IModelAdministration.h) |
| admin | transport | `AdminHttp` | [AdminHttp.cpp](../../../backend/application/modules/admin/transport/AdminHttp/AdminHttp.cpp), [AdminHttp.h](../../../backend/application/modules/admin/transport/AdminHttp/AdminHttp.h) |
| admin | transport | `ModelAdminHttp` | [ModelAdminHttp.cpp](../../../backend/application/modules/admin/transport/ModelAdminHttp/ModelAdminHttp.cpp), [ModelAdminHttp.h](../../../backend/application/modules/admin/transport/ModelAdminHttp/ModelAdminHttp.h) |
| announcement | application | `AnnouncementService` | [AnnouncementService.cpp](../../../backend/application/modules/announcement/application/AnnouncementService/AnnouncementService.cpp), [AnnouncementService.h](../../../backend/application/modules/announcement/application/AnnouncementService/AnnouncementService.h) |
| announcement | infrastructure | `AnnouncementPostgresRepository` | [AnnouncementPostgresRepository.cpp](../../../backend/application/modules/announcement/infrastructure/AnnouncementPostgresRepository/AnnouncementPostgresRepository.cpp), [AnnouncementPostgresRepository.h](../../../backend/application/modules/announcement/infrastructure/AnnouncementPostgresRepository/AnnouncementPostgresRepository.h) |
| announcement | ports | `IAnnouncementRepository` | [IAnnouncementRepository.h](../../../backend/application/modules/announcement/ports/IAnnouncementRepository/IAnnouncementRepository.h) |
| announcement | transport | `AnnouncementHttp` | [AnnouncementHttp.cpp](../../../backend/application/modules/announcement/transport/AnnouncementHttp/AnnouncementHttp.cpp), [AnnouncementHttp.h](../../../backend/application/modules/announcement/transport/AnnouncementHttp/AnnouncementHttp.h) |
| bootstrap | Function3Runtime | `Function3Runtime` | [Function3Runtime.cpp](../../../backend/application/modules/bootstrap/Function3Runtime/Function3Runtime.cpp), [Function3Runtime.h](../../../backend/application/modules/bootstrap/Function3Runtime/Function3Runtime.h) |
| messaging | application | `MessagingService` | [MessagingService.cpp](../../../backend/application/modules/messaging/application/MessagingService/MessagingService.cpp), [MessagingService.h](../../../backend/application/modules/messaging/application/MessagingService/MessagingService.h) |
| messaging | infrastructure | `ChatOutboxWriter` | [ChatOutboxWriter.cpp](../../../backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.cpp), [ChatOutboxWriter.h](../../../backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.h) |
| messaging | infrastructure | `PostgresEventPipeline` | [PostgresEventPipeline.cpp](../../../backend/application/modules/messaging/infrastructure/PostgresEventPipeline/PostgresEventPipeline.cpp), [PostgresEventPipeline.h](../../../backend/application/modules/messaging/infrastructure/PostgresEventPipeline/PostgresEventPipeline.h) |
| messaging | infrastructure | `PostgresEventWriter` | [PostgresEventWriter.cpp](../../../backend/application/modules/messaging/infrastructure/PostgresEventWriter/PostgresEventWriter.cpp), [PostgresEventWriter.h](../../../backend/application/modules/messaging/infrastructure/PostgresEventWriter/PostgresEventWriter.h) |
| messaging | infrastructure | `RocketMqBroker` | [RocketMqBroker.cpp](../../../backend/application/modules/messaging/infrastructure/RocketMqBroker/RocketMqBroker.cpp), [RocketMqBroker.h](../../../backend/application/modules/messaging/infrastructure/RocketMqBroker/RocketMqBroker.h) |
| messaging | ports | `IEventBroker` | [IEventBroker.h](../../../backend/application/modules/messaging/ports/IEventBroker/IEventBroker.h) |
| messaging | ports | `IMessagingOperations` | [IMessagingOperations.h](../../../backend/application/modules/messaging/ports/IMessagingOperations/IMessagingOperations.h) |
| messaging | transport | `MessagingHttp` | [MessagingHttp.cpp](../../../backend/application/modules/messaging/transport/MessagingHttp/MessagingHttp.cpp), [MessagingHttp.h](../../../backend/application/modules/messaging/transport/MessagingHttp/MessagingHttp.h) |
| moderation | application | `ModerationService` | [ModerationService.cpp](../../../backend/application/modules/moderation/application/ModerationService/ModerationService.cpp), [ModerationService.h](../../../backend/application/modules/moderation/application/ModerationService/ModerationService.h) |
| moderation | infrastructure | `ReportPostgresRepository` | [ReportPostgresRepository.cpp](../../../backend/application/modules/moderation/infrastructure/ReportPostgresRepository/ReportPostgresRepository.cpp), [ReportPostgresRepository.h](../../../backend/application/modules/moderation/infrastructure/ReportPostgresRepository/ReportPostgresRepository.h) |
| moderation | ports | `IReportRepository` | [IReportRepository.h](../../../backend/application/modules/moderation/ports/IReportRepository/IReportRepository.h) |
| ranking | application | `RankingService` | [RankingService.cpp](../../../backend/application/modules/ranking/application/RankingService/RankingService.cpp), [RankingService.h](../../../backend/application/modules/ranking/application/RankingService/RankingService.h) |
| ranking | domain | `Ranking` | [Ranking.h](../../../backend/application/modules/ranking/domain/Ranking/Ranking.h) |
| ranking | infrastructure | `RankingPostgresRepository` | [RankingPostgresRepository.cpp](../../../backend/application/modules/ranking/infrastructure/RankingPostgresRepository/RankingPostgresRepository.cpp), [RankingPostgresRepository.h](../../../backend/application/modules/ranking/infrastructure/RankingPostgresRepository/RankingPostgresRepository.h) |
| ranking | ports | `IRankingRepository` | [IRankingRepository.h](../../../backend/application/modules/ranking/ports/IRankingRepository/IRankingRepository.h) |
| shared | BusinessContext | `BusinessContext` | [BusinessContext.h](../../../backend/application/modules/shared/BusinessContext/BusinessContext.h) |
| shared | infrastructure | `Phase11Bridge` | [Phase11Bridge.cpp](../../../backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.cpp), [Phase11Bridge.h](../../../backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h) |
| shared | infrastructure | `ProcessRunner` | [ProcessRunner.cpp](../../../backend/application/modules/shared/infrastructure/ProcessRunner/ProcessRunner.cpp), [ProcessRunner.h](../../../backend/application/modules/shared/infrastructure/ProcessRunner/ProcessRunner.h) |
| shared | infrastructure | `SqlContext` | [SqlContext.cpp](../../../backend/application/modules/shared/infrastructure/SqlContext/SqlContext.cpp), [SqlContext.h](../../../backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h) |
| shared | TextRules | `TextRules` | [TextRules.h](../../../backend/application/modules/shared/TextRules/TextRules.h) |
| shared | transport | `BusinessHttp` | [BusinessHttp.h](../../../backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h) |
| shared | transport | `FrontendAssets` | [FrontendAssets.cpp](../../../backend/application/modules/shared/transport/FrontendAssets/FrontendAssets.cpp), [FrontendAssets.h](../../../backend/application/modules/shared/transport/FrontendAssets/FrontendAssets.h) |
| speech | application | `SpeechService` | [SpeechService.cpp](../../../backend/ai/speech/application/SpeechService/SpeechService.cpp), [SpeechService.h](../../../backend/ai/speech/application/SpeechService/SpeechService.h) |
| speech | infrastructure | `SherpaSpeechWorker` | [SherpaSpeechWorker.cpp](../../../backend/ai/speech/infrastructure/SherpaSpeechWorker/SherpaSpeechWorker.cpp) |
| speech | infrastructure | `SpeechPostgresTasks` | [SpeechPostgresTasks.cpp](../../../backend/ai/speech/infrastructure/SpeechPostgresTasks/SpeechPostgresTasks.cpp), [SpeechPostgresTasks.h](../../../backend/ai/speech/infrastructure/SpeechPostgresTasks/SpeechPostgresTasks.h) |
| speech | infrastructure | `SpeechProcessRenderer` | [SpeechProcessRenderer.cpp](../../../backend/ai/speech/infrastructure/SpeechProcessRenderer/SpeechProcessRenderer.cpp), [SpeechProcessRenderer.h](../../../backend/ai/speech/infrastructure/SpeechProcessRenderer/SpeechProcessRenderer.h) |
| speech | infrastructure | `SpeechRuntime` | [SpeechRuntime.cpp](../../../backend/ai/speech/infrastructure/SpeechRuntime/SpeechRuntime.cpp), [SpeechRuntime.h](../../../backend/ai/speech/infrastructure/SpeechRuntime/SpeechRuntime.h) |
| speech | ports | `ISpeechTasks` | [ISpeechTasks.h](../../../backend/ai/speech/ports/ISpeechTasks/ISpeechTasks.h) |
| speech | transport | `SpeechAdminHttp` | [SpeechAdminHttp.cpp](../../../backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.cpp), [SpeechAdminHttp.h](../../../backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.h) |
| speech | transport | `SpeechHttp` | [SpeechHttp.cpp](../../../backend/ai/speech/transport/SpeechHttp/SpeechHttp.cpp), [SpeechHttp.h](../../../backend/ai/speech/transport/SpeechHttp/SpeechHttp.h) |
| tree | application | `TreeMediaService` | [TreeMediaService.cpp](../../../backend/application/modules/tree/application/TreeMediaService/TreeMediaService.cpp), [TreeMediaService.h](../../../backend/application/modules/tree/application/TreeMediaService/TreeMediaService.h) |
| tree | application | `TreeService` | [TreeService.cpp](../../../backend/application/modules/tree/application/TreeService/TreeService.cpp), [TreeService.h](../../../backend/application/modules/tree/application/TreeService/TreeService.h) |
| tree | domain | `TreePost` | [TreePost.h](../../../backend/application/modules/tree/domain/TreePost/TreePost.h) |
| tree | domain | `TreeReply` | [TreeReply.h](../../../backend/application/modules/tree/domain/TreeReply/TreeReply.h) |
| tree | infrastructure | `FfmpegMediaProcessor` | [FfmpegMediaProcessor.cpp](../../../backend/application/modules/tree/infrastructure/FfmpegMediaProcessor/FfmpegMediaProcessor.cpp), [FfmpegMediaProcessor.h](../../../backend/application/modules/tree/infrastructure/FfmpegMediaProcessor/FfmpegMediaProcessor.h) |
| tree | infrastructure | `InMemoryTreeRepository` | [InMemoryTreeRepository.cpp](../../../backend/application/modules/tree/infrastructure/InMemoryTreeRepository/InMemoryTreeRepository.cpp), [InMemoryTreeRepository.h](../../../backend/application/modules/tree/infrastructure/InMemoryTreeRepository/InMemoryTreeRepository.h) |
| tree | infrastructure | `TreeMediaPostgresRepository` | [TreeMediaPostgresRepository.cpp](../../../backend/application/modules/tree/infrastructure/TreeMediaPostgresRepository/TreeMediaPostgresRepository.cpp), [TreeMediaPostgresRepository.h](../../../backend/application/modules/tree/infrastructure/TreeMediaPostgresRepository/TreeMediaPostgresRepository.h) |
| tree | infrastructure | `TreePostgresRepository` | [TreePostgresRepository.cpp](../../../backend/application/modules/tree/infrastructure/TreePostgresRepository/TreePostgresRepository.cpp), [TreePostgresRepository.h](../../../backend/application/modules/tree/infrastructure/TreePostgresRepository/TreePostgresRepository.h) |
| tree | infrastructure | `TreeRankingSource` | [TreeRankingSource.cpp](../../../backend/application/modules/tree/infrastructure/TreeRankingSource/TreeRankingSource.cpp), [TreeRankingSource.h](../../../backend/application/modules/tree/infrastructure/TreeRankingSource/TreeRankingSource.h) |
| tree | ports | `ITreeMediaProcessor` | [ITreeMediaProcessor.h](../../../backend/application/modules/tree/ports/ITreeMediaProcessor/ITreeMediaProcessor.h) |
| tree | ports | `ITreeRepository` | [ITreeRepository.h](../../../backend/application/modules/tree/ports/ITreeRepository/ITreeRepository.h) |
| tree | transport | `TreeHttp` | [TreeHttp.cpp](../../../backend/application/modules/tree/transport/TreeHttp/TreeHttp.cpp), [TreeHttp.h](../../../backend/application/modules/tree/transport/TreeHttp/TreeHttp.h) |
| tree | transport | `TreeMediaHttp` | [TreeMediaHttp.cpp](../../../backend/application/modules/tree/transport/TreeMediaHttp/TreeMediaHttp.cpp), [TreeMediaHttp.h](../../../backend/application/modules/tree/transport/TreeMediaHttp/TreeMediaHttp.h) |

## 前端

- `frontend/phase11/tree/`：TreeApp、TreeScene、VoiceRecorder。
- `frontend/phase11/admin/AdminApp/`：角色隔离的管理入口。
- `frontend/phase11/shared/`：BusinessApi、RecoveryBudget、AnnouncementPlayer。
- `frontend/phase11/shared/AppNavigation/`：聊天、神树、后台统一导航与声音设置容器。
- `frontend/phase11/chat/ChatWorkbench/`：会话/消息工作区布局与移动适配。
- `frontend/phase11/chat/VirtualMessageList/`：长会话有界渲染。
- `frontend/phase11/vendor/three-r186/`：固定模块、控件、许可与制品摘要。

## 复现工具

- `scripts/function3/dependencies/`：显式批准的下载、已有制品准备、隔离 Broker。
- `scripts/function3/validation/`：源码快照、隔离数据库/服务、三轮性能矩阵。
- `tests/function3/unit/`、`integration/`、`frontend/`、`architecture/`：分层验证。
- `benchmarks/function3_mixed.py`：持久化 ACK 与消息到达分开计量。

依赖方向、线程与事务解释见 Phase28–33，实际测试结果见 [Phase34](phase34.md)。

## 最后验证与依赖补丁索引

- `scripts/function3/dependencies/reuse_sdk.py`：先核验既有源码与协议，无网络；显式要求才重建。
- `scripts/function3/dependencies/patches/SdkCompatibility/`：SDK 全部修改的补丁与逐文件摘要。
- `scripts/function3/dependencies/patches/FifoProducerPartition/`：FIFO 最终失败必须回调的策略。
- `scripts/function3/validation/final_checks.py`：带标记/PID 校验的隔离收尾验证。
- `scripts/function3/validation/build_checks.py`：复用依赖的 Debug、ASan/LSan/UBSan 检查。
- `scripts/function3/validation/collect_evidence.py`：白名单证据归档，不收集密钥、模型或上传内容。
- `tests/function3/integration/http2_business.py`：证书校验、ALPN、业务流及相邻流取消。
- `tests/function3/integration/ai_mixed.py`：已有真实 ONNX 模型的后台启用与 AI/普通聊天并行验证。
- `tests/function3/frontend/create_webm_fixture.cjs`：本地合成浏览器录音格式测试素材，不使用真实麦克风。

后台模型发布与语音试听都采用短 HTTP 请求 + 有界后台任务 + 状态查询，不占住一条 HTTP 请求等待冷模型加载。

存储专用策略接口：[PostgresOutboxWriter.h](../../../backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h)。它只在驱动与装配层使用，公共业务 Port 不暴露 PGconn。

事务内聊天移交实现：[ChatOutboxWriter.h](../../../backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.h)、[ChatOutboxWriter.cpp](../../../backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.cpp)。该组件不持有业务线程的PG连接，事务提交后通知只拥有独立共享信号状态。

## Phase35 增量

| 模块 | 职责 | 文件 |
|---|---|---|
| cache/ports | 可丢弃读缓存端口，不暴露Redis类型 | [IQueryCache.h](../../../backend/application/modules/cache/ports/IQueryCache/IQueryCache.h) |
| cache/infrastructure | 有界RESP2、代次失效、TTL与故障旁路 | [RedisReadCache.h](../../../backend/application/modules/cache/infrastructure/RedisReadCache/RedisReadCache.h)、[RedisReadCache.cpp](../../../backend/application/modules/cache/infrastructure/RedisReadCache/RedisReadCache.cpp) |
| messaging/infrastructure | 无外部Broker时复用持久发布/Inbox消费 | [LocalEventBroker.h](../../../backend/application/modules/messaging/infrastructure/LocalEventBroker/LocalEventBroker.h)、[LocalEventBroker.cpp](../../../backend/application/modules/messaging/infrastructure/LocalEventBroker/LocalEventBroker.cpp) |
| scripts/runtime | 原库备份、迁移摘要、原密钥、18090与进程归属 | [service.py](../../../scripts/function3/runtime/service.py) |

语音、回复和前端具体改动、调用图及验证入口见[Phase35](phase35.md)。

I/O诊断脚本：[wal_diagnostic.py](../../../scripts/function3/validation/wal_diagnostic.py)，只调整带标记的私有PG集群，结束后恢复计时配置。
