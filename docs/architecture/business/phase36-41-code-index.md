# Phase36–41组件与文件索引

本索引按实际目录生成，同名组件的头文件和实现放在一起。纯接口只有头文件。调用链、线程、权限与事务见[总图与讲义](phase36-41-end-to-end.md)。

## Windows本机入口与私人业务

| 组件 | 文件 |
|---|---|
| `client/assistant/ActionService` | [ActionService.cpp](../../../client/assistant/ActionService/ActionService.cpp) · [ActionService.h](../../../client/assistant/ActionService/ActionService.h) |
| `client/assistant/DocumentImport` | [DocumentImport.cpp](../../../client/assistant/DocumentImport/DocumentImport.cpp) · [DocumentImport.h](../../../client/assistant/DocumentImport/DocumentImport.h) |
| `client/assistant/LocalAssistantService` | [LocalAssistantService.cpp](../../../client/assistant/LocalAssistantService/LocalAssistantService.cpp) · [LocalAssistantService.h](../../../client/assistant/LocalAssistantService/LocalAssistantService.h) |
| `client/assistant/LocalKnowledge` | [LocalKnowledge.cpp](../../../client/assistant/LocalKnowledge/LocalKnowledge.cpp) · [LocalKnowledge.h](../../../client/assistant/LocalKnowledge/LocalKnowledge.h) |
| `client/assistant/PersonalSpeech` | [PersonalSpeech.cpp](../../../client/assistant/PersonalSpeech/PersonalSpeech.cpp) · [PersonalSpeech.h](../../../client/assistant/PersonalSpeech/PersonalSpeech.h) |
| `client/assistant/PrivateHistory` | [PrivateHistory.cpp](../../../client/assistant/PrivateHistory/PrivateHistory.cpp) · [PrivateHistory.h](../../../client/assistant/PrivateHistory/PrivateHistory.h) |
| `client/assistant/PrivateMigration` | [PrivateMigration.cpp](../../../client/assistant/PrivateMigration/PrivateMigration.cpp) · [PrivateMigration.h](../../../client/assistant/PrivateMigration/PrivateMigration.h) |
| `client/assistant/WindowsCloudSpeech` | [WindowsCloudSpeech.cpp](../../../client/assistant/WindowsCloudSpeech/WindowsCloudSpeech.cpp) · [WindowsCloudSpeech.h](../../../client/assistant/WindowsCloudSpeech/WindowsCloudSpeech.h) |
| `client/companion/BoundedPool` | [BoundedPool.cpp](../../../client/companion/BoundedPool/BoundedPool.cpp) · [BoundedPool.h](../../../client/companion/BoundedPool/BoundedPool.h) |
| `client/companion/CompanionMain` | [CompanionMain.cpp](../../../client/companion/CompanionMain/CompanionMain.cpp) |
| `client/companion/JsonRuntime` | [JsonRuntime.cpp](../../../client/companion/JsonRuntime/JsonRuntime.cpp) |
| `client/companion/LoopbackPolicy` | [LoopbackPolicy.cpp](../../../client/companion/LoopbackPolicy/LoopbackPolicy.cpp) · [LoopbackPolicy.h](../../../client/companion/LoopbackPolicy/LoopbackPolicy.h) |
| `client/companion/ProxyServer` | [ProxyServer.cpp](../../../client/companion/ProxyServer/ProxyServer.cpp) · [ProxyServer.h](../../../client/companion/ProxyServer/ProxyServer.h) · [ProxyStreams.ipp](../../../client/companion/ProxyServer/ProxyStreams.ipp) |
| `client/companion/SChannelStream` | [SChannelStream.cpp](../../../client/companion/SChannelStream/SChannelStream.cpp) · [SChannelStream.h](../../../client/companion/SChannelStream/SChannelStream.h) |
| `client/companion/UpstreamTransport` | [UpstreamTransport.cpp](../../../client/companion/UpstreamTransport/UpstreamTransport.cpp) · [UpstreamTransport.h](../../../client/companion/UpstreamTransport/UpstreamTransport.h) |
| `client/storage/CredentialVault` | [CredentialVault.cpp](../../../client/storage/CredentialVault/CredentialVault.cpp) · [CredentialVault.h](../../../client/storage/CredentialVault/CredentialVault.h) |
| `client/storage/SqliteStore` | [SqliteStore.cpp](../../../client/storage/SqliteStore/SqliteStore.cpp) · [SqliteStore.h](../../../client/storage/SqliteStore/SqliteStore.h) |
| `client/storage/WindowsProtection` | [WindowsProtection.cpp](../../../client/storage/WindowsProtection/WindowsProtection.cpp) · [WindowsProtection.h](../../../client/storage/WindowsProtection/WindowsProtection.h) |

## 私人推理核心

| 组件 | 文件 |
|---|---|
| `backend/ai/private/application/PrivateGenerationService` | [PrivateGenerationService.cpp](../../../backend/ai/private/application/PrivateGenerationService/PrivateGenerationService.cpp) · [PrivateGenerationService.h](../../../backend/ai/private/application/PrivateGenerationService/PrivateGenerationService.h) |
| `backend/ai/private/infrastructure/PrivateModelRouter` | [PrivateModelRouter.cpp](../../../backend/ai/private/infrastructure/PrivateModelRouter/PrivateModelRouter.cpp) · [PrivateModelRouter.h](../../../backend/ai/private/infrastructure/PrivateModelRouter/PrivateModelRouter.h) |
| `backend/ai/private/ports/IPrivateInference` | [IPrivateInference.h](../../../backend/ai/private/ports/IPrivateInference/IPrivateInference.h) |
| `backend/ai/private/transport/PrivateGenerationHttp` | [PrivateGenerationHttp.cpp](../../../backend/ai/private/transport/PrivateGenerationHttp/PrivateGenerationHttp.cpp) · [PrivateGenerationHttp.h](../../../backend/ai/private/transport/PrivateGenerationHttp/PrivateGenerationHttp.h) |

## 语音

| 组件 | 文件 |
|---|---|
| `backend/ai/speech/application/CloudSpeechPolicy` | [CloudSpeechPolicy.cpp](../../../backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.cpp) · [CloudSpeechPolicy.h](../../../backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.h) |
| `backend/ai/speech/application/SpeechService` | [SpeechService.cpp](../../../backend/ai/speech/application/SpeechService/SpeechService.cpp) · [SpeechService.h](../../../backend/ai/speech/application/SpeechService/SpeechService.h) |
| `backend/ai/speech/application/SpeechText` | [SpeechText.cpp](../../../backend/ai/speech/application/SpeechText/SpeechText.cpp) · [SpeechText.h](../../../backend/ai/speech/application/SpeechText/SpeechText.h) |
| `backend/ai/speech/application/SpeechWire` | [SpeechWire.cpp](../../../backend/ai/speech/application/SpeechWire/SpeechWire.cpp) · [SpeechWire.h](../../../backend/ai/speech/application/SpeechWire/SpeechWire.h) |
| `backend/ai/speech/application/WaveCodec` | [WaveCodec.cpp](../../../backend/ai/speech/application/WaveCodec/WaveCodec.cpp) · [WaveCodec.h](../../../backend/ai/speech/application/WaveCodec/WaveCodec.h) |
| `backend/ai/speech/domain/SpeechProfile` | [SpeechProfile.h](../../../backend/ai/speech/domain/SpeechProfile/SpeechProfile.h) |
| `backend/ai/speech/infrastructure/CurlCloudSpeech` | [CurlCloudSpeech.cpp](../../../backend/ai/speech/infrastructure/CurlCloudSpeech/CurlCloudSpeech.cpp) · [CurlCloudSpeech.h](../../../backend/ai/speech/infrastructure/CurlCloudSpeech/CurlCloudSpeech.h) |
| `backend/ai/speech/infrastructure/SherpaSpeechWorker` | [SherpaSpeechWorker.cpp](../../../backend/ai/speech/infrastructure/SherpaSpeechWorker/SherpaSpeechWorker.cpp) |
| `backend/ai/speech/infrastructure/SiteSpeechRenderer` | [SiteSpeechRenderer.cpp](../../../backend/ai/speech/infrastructure/SiteSpeechRenderer/SiteSpeechRenderer.cpp) · [SiteSpeechRenderer.h](../../../backend/ai/speech/infrastructure/SiteSpeechRenderer/SiteSpeechRenderer.h) |
| `backend/ai/speech/infrastructure/SpeechPostgresTasks` | [SpeechPostgresTasks.cpp](../../../backend/ai/speech/infrastructure/SpeechPostgresTasks/SpeechPostgresTasks.cpp) · [SpeechPostgresTasks.h](../../../backend/ai/speech/infrastructure/SpeechPostgresTasks/SpeechPostgresTasks.h) |
| `backend/ai/speech/infrastructure/SpeechProcessRenderer` | [SpeechProcessRenderer.cpp](../../../backend/ai/speech/infrastructure/SpeechProcessRenderer/SpeechProcessRenderer.cpp) · [SpeechProcessRenderer.h](../../../backend/ai/speech/infrastructure/SpeechProcessRenderer/SpeechProcessRenderer.h) |
| `backend/ai/speech/infrastructure/SpeechRuntime` | [SpeechRuntime.cpp](../../../backend/ai/speech/infrastructure/SpeechRuntime/SpeechRuntime.cpp) · [SpeechRuntime.h](../../../backend/ai/speech/infrastructure/SpeechRuntime/SpeechRuntime.h) |
| `backend/ai/speech/ports/ICloudSpeech` | [ICloudSpeech.h](../../../backend/ai/speech/ports/ICloudSpeech/ICloudSpeech.h) |
| `backend/ai/speech/ports/ISpeechControl` | [ISpeechControl.h](../../../backend/ai/speech/ports/ISpeechControl/ISpeechControl.h) |
| `backend/ai/speech/ports/ISpeechTasks` | [ISpeechTasks.h](../../../backend/ai/speech/ports/ISpeechTasks/ISpeechTasks.h) |
| `backend/ai/speech/transport/SpeechAdminHttp` | [SpeechAdminHttp.cpp](../../../backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.cpp) · [SpeechAdminHttp.h](../../../backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.h) |
| `backend/ai/speech/transport/SpeechHttp` | [SpeechHttp.cpp](../../../backend/ai/speech/transport/SpeechHttp/SpeechHttp.cpp) · [SpeechHttp.h](../../../backend/ai/speech/transport/SpeechHttp/SpeechHttp.h) |

## 统一动态

| 组件 | 文件 |
|---|---|
| `backend/application/modules/activities/application/ActivityService` | [ActivityService.cpp](../../../backend/application/modules/activities/application/ActivityService/ActivityService.cpp) · [ActivityService.h](../../../backend/application/modules/activities/application/ActivityService/ActivityService.h) |
| `backend/application/modules/activities/infrastructure/ActivityPostgresViews` | [ActivityPostgresViews.cpp](../../../backend/application/modules/activities/infrastructure/ActivityPostgresViews/ActivityPostgresViews.cpp) · [ActivityPostgresViews.h](../../../backend/application/modules/activities/infrastructure/ActivityPostgresViews/ActivityPostgresViews.h) |
| `backend/application/modules/activities/ports/IActivityBookmarks` | [IActivityBookmarks.h](../../../backend/application/modules/activities/ports/IActivityBookmarks/IActivityBookmarks.h) |
| `backend/application/modules/activities/ports/IActivityContent` | [IActivityContent.h](../../../backend/application/modules/activities/ports/IActivityContent/IActivityContent.h) |
| `backend/application/modules/activities/ports/IContentAudience` | [IContentAudience.h](../../../backend/application/modules/activities/ports/IContentAudience/IContentAudience.h) |
| `backend/application/modules/activities/ports/IProfileDirectory` | [IProfileDirectory.h](../../../backend/application/modules/activities/ports/IProfileDirectory/IProfileDirectory.h) |
| `backend/application/modules/activities/transport/ActivityHttp` | [ActivityHttp.cpp](../../../backend/application/modules/activities/transport/ActivityHttp/ActivityHttp.cpp) · [ActivityHttp.h](../../../backend/application/modules/activities/transport/ActivityHttp/ActivityHttp.h) |
| `backend/application/modules/activities/transport/ActivityJson` | [ActivityJson.cpp](../../../backend/application/modules/activities/transport/ActivityJson/ActivityJson.cpp) · [ActivityJson.h](../../../backend/application/modules/activities/transport/ActivityJson/ActivityJson.h) |

## 群AI与迁移

| 组件 | 文件 |
|---|---|
| `backend/application/modules/assistant/application/AiTriggerPolicy` | [AiTriggerPolicy.cpp](../../../backend/application/modules/assistant/application/AiTriggerPolicy/AiTriggerPolicy.cpp) · [AiTriggerPolicy.h](../../../backend/application/modules/assistant/application/AiTriggerPolicy/AiTriggerPolicy.h) |
| `backend/application/modules/assistant/application/PrivateArchiveService` | [PrivateArchiveService.cpp](../../../backend/application/modules/assistant/application/PrivateArchiveService/PrivateArchiveService.cpp) · [PrivateArchiveService.h](../../../backend/application/modules/assistant/application/PrivateArchiveService/PrivateArchiveService.h) |
| `backend/application/modules/assistant/application/PrivateModelDirectory` | [PrivateModelDirectory.cpp](../../../backend/application/modules/assistant/application/PrivateModelDirectory/PrivateModelDirectory.cpp) · [PrivateModelDirectory.h](../../../backend/application/modules/assistant/application/PrivateModelDirectory/PrivateModelDirectory.h) |
| `backend/application/modules/assistant/domain/PrivateArchive` | [PrivateArchive.h](../../../backend/application/modules/assistant/domain/PrivateArchive/PrivateArchive.h) |
| `backend/application/modules/assistant/infrastructure/AssistantModelsPostgres` | [AssistantModelsPostgres.cpp](../../../backend/application/modules/assistant/infrastructure/AssistantModelsPostgres/AssistantModelsPostgres.cpp) · [AssistantModelsPostgres.h](../../../backend/application/modules/assistant/infrastructure/AssistantModelsPostgres/AssistantModelsPostgres.h) |
| `backend/application/modules/assistant/infrastructure/LegacyArchivePostgres` | [LegacyArchivePostgres.cpp](../../../backend/application/modules/assistant/infrastructure/LegacyArchivePostgres/LegacyArchivePostgres.cpp) · [LegacyArchivePostgres.h](../../../backend/application/modules/assistant/infrastructure/LegacyArchivePostgres/LegacyArchivePostgres.h) |
| `backend/application/modules/assistant/ports/IAssistantModels` | [IAssistantModels.h](../../../backend/application/modules/assistant/ports/IAssistantModels/IAssistantModels.h) |
| `backend/application/modules/assistant/ports/IPrivateArchive` | [IPrivateArchive.h](../../../backend/application/modules/assistant/ports/IPrivateArchive/IPrivateArchive.h) |
| `backend/application/modules/assistant/transport/AssistantModelsHttp` | [AssistantModelsHttp.cpp](../../../backend/application/modules/assistant/transport/AssistantModelsHttp/AssistantModelsHttp.cpp) · [AssistantModelsHttp.h](../../../backend/application/modules/assistant/transport/AssistantModelsHttp/AssistantModelsHttp.h) |
| `backend/application/modules/assistant/transport/PrivateArchiveHttp` | [PrivateArchiveHttp.cpp](../../../backend/application/modules/assistant/transport/PrivateArchiveHttp/PrivateArchiveHttp.cpp) · [PrivateArchiveHttp.h](../../../backend/application/modules/assistant/transport/PrivateArchiveHttp/PrivateArchiveHttp.h) |

## 资料幂等端口

| 组件 | 文件 |
|---|---|
| `backend/application/modules/profiles/ports/IProfileOperations` | [IProfileOperations.h](../../../backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h) |

## 前端与装配

| 区域 | 入口 |
|---|---|
| Linux装配根 | [main.cpp](../../../main.cpp) |
| Function3装配 | [Function3Runtime.cpp](../../../backend/application/modules/bootstrap/Function3Runtime/Function3Runtime.cpp) |
| 原生客户端装配 | [CompanionMain.cpp](../../../client/companion/CompanionMain/CompanionMain.cpp) |
| Windows独立构建 | [CMakeLists.txt](../../../client/CMakeLists.txt) |
| 全局与好友式助手 | [AssistantPanel.js](../../../frontend/phase11/shared/AssistantPanel/AssistantPanel.js) |
| 好友式助手页面 | [index.html](../../../frontend/phase11/assistant/index.html) |
| 本机引导与跨页身份 | [ClientBootstrap.js](../../../frontend/phase11/shared/ClientBootstrap/ClientBootstrap.js) |
| 语音设置 | [SpeechSettings.js](../../../frontend/phase11/shared/SpeechSettings/SpeechSettings.js) |
| 批量会话摘要及PG存储 | [PostgresStore.cpp](../../../backend/application/storage/postgres/PostgresStore/PostgresStore.cpp) |
| 统一动态发布器 | [ActivityPublisher.js](../../../frontend/phase11/shared/ActivityPublisher/ActivityPublisher.js) |
| 统一动态详情 | [ActivityDetail.js](../../../frontend/phase11/shared/ActivityDetail/ActivityDetail.js) |

## 验证入口

- Linux构建：`scripts/function3/validation/build_checks.py`；原始Phase35快照构建：`prepare_phase35_baseline.py`。
- 三轮对照与诊断：`scripts/function3/validation/phase41_benchmark.py`、`benchmarks/phase41_database_observer.py`。
- Windows构建：`scripts/client/build_windows.py`；真实TLS/代理/账号/RAG/Agent/语音/资源：`tests/client/native_integration.py`。
- 活动权限、旧资料迁移、语音、迁移回滚和独立Worker：`tests/function3/integration/*_blackbox.py`。
- 固定公开文案试听的WAV校验不等于自然度合格，教学模型链路验证不等于真实训练质量评测。

## Phase42增量（分区不变）

- 普通TLS请求复用：[HttpConnectionPool](../../../client/companion/HttpConnectionPool/HttpConnectionPool.h)；在现有`UpstreamTransport`内装配。
- 静态树干几何合并：[WoodGeometry.js](../../../frontend/phase11/tree/TreeScene/WoodGeometry.js)，属于现有`TreeScene`的小型辅助实现。
- 批量读取仍由原`TreeService → ITreeRepository → TreePostgresRepository`处理，没有增加跨模块数据库依赖；详见[Phase42](phase42.md)。
