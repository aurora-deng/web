# Phase48 组件索引

同名目录保留同名实现/接口；纯类型、端口和模板头文件没有创建空cpp。Domain、Ports及旧Store继续充当兼容入口。

|区域|组件|文件|
|---|---|---|
|领域类型|AccountTypes|[AccountTypes.h](../../../backend/application/domain/AccountTypes/AccountTypes.h)|
|领域类型|ConversationTypes|[ConversationTypes.h](../../../backend/application/domain/ConversationTypes/ConversationTypes.h)|
|领域类型|Domain|[Domain.h](../../../backend/application/domain/Domain/Domain.h)|
|领域类型|EvaluationTypes|[EvaluationTypes.h](../../../backend/application/domain/EvaluationTypes/EvaluationTypes.h)|
|领域类型|FileTypes|[FileTypes.h](../../../backend/application/domain/FileTypes/FileTypes.h)|
|领域类型|GenerationTypes|[GenerationTypes.h](../../../backend/application/domain/GenerationTypes/GenerationTypes.h)|
|领域类型|IdentityTypes|[IdentityTypes.h](../../../backend/application/domain/IdentityTypes/IdentityTypes.h)|
|领域类型|KnowledgeTypes|[KnowledgeTypes.h](../../../backend/application/domain/KnowledgeTypes/KnowledgeTypes.h)|
|领域类型|MessageTypes|[MessageTypes.h](../../../backend/application/domain/MessageTypes/MessageTypes.h)|
|领域类型|ModelTypes|[ModelTypes.h](../../../backend/application/domain/ModelTypes/ModelTypes.h)|
|领域类型|OutboxTypes|[OutboxTypes.h](../../../backend/application/domain/OutboxTypes/OutboxTypes.h)|
|领域类型|SocialTypes|[SocialTypes.h](../../../backend/application/domain/SocialTypes/SocialTypes.h)|
|领域类型|TrainingTypes|[TrainingTypes.h](../../../backend/application/domain/TrainingTypes/TrainingTypes.h)|
|专用端口|IAiGenerationAuditRepository|[IAiGenerationAuditRepository.h](../../../backend/application/ports/IAiGenerationAuditRepository/IAiGenerationAuditRepository.h)|
|专用端口|IConversationRepository|[IConversationRepository.h](../../../backend/application/ports/IConversationRepository/IConversationRepository.h)|
|专用端口|ICredentialCodec|[ICredentialCodec.h](../../../backend/application/ports/ICredentialCodec/ICredentialCodec.h)|
|专用端口|IDatabase|[IDatabase.h](../../../backend/application/ports/IDatabase/IDatabase.h)|
|专用端口|IFileRepository|[IFileRepository.h](../../../backend/application/ports/IFileRepository/IFileRepository.h)|
|专用端口|IFileStorage|[IFileStorage.h](../../../backend/application/ports/IFileStorage/IFileStorage.h)|
|专用端口|IKnowledgeIndex|[IKnowledgeIndex.h](../../../backend/application/ports/IKnowledgeIndex/IKnowledgeIndex.h)|
|专用端口|IKnowledgeRepository|[IKnowledgeRepository.h](../../../backend/application/ports/IKnowledgeRepository/IKnowledgeRepository.h)|
|专用端口|ILoginRateLimiter|[ILoginRateLimiter.h](../../../backend/application/ports/ILoginRateLimiter/ILoginRateLimiter.h)|
|专用端口|IMessageRepository|[IMessageRepository.h](../../../backend/application/ports/IMessageRepository/IMessageRepository.h)|
|专用端口|IModelEvaluationRepository|[IModelEvaluationRepository.h](../../../backend/application/ports/IModelEvaluationRepository/IModelEvaluationRepository.h)|
|专用端口|IModelQualityGate|[IModelQualityGate.h](../../../backend/application/ports/IModelQualityGate/IModelQualityGate.h)|
|专用端口|IModelRepository|[IModelRepository.h](../../../backend/application/ports/IModelRepository/IModelRepository.h)|
|专用端口|IOutboxRepository|[IOutboxRepository.h](../../../backend/application/ports/IOutboxRepository/IOutboxRepository.h)|
|专用端口|IRagContextProvider|[IRagContextProvider.h](../../../backend/application/ports/IRagContextProvider/IRagContextProvider.h)|
|专用端口|ISessionRepository|[ISessionRepository.h](../../../backend/application/ports/ISessionRepository/ISessionRepository.h)|
|专用端口|ISocialRepository|[ISocialRepository.h](../../../backend/application/ports/ISocialRepository/ISocialRepository.h)|
|专用端口|ITrainingRepository|[ITrainingRepository.h](../../../backend/application/ports/ITrainingRepository/ITrainingRepository.h)|
|专用端口|ITransaction|[ITransaction.h](../../../backend/application/ports/ITransaction/ITransaction.h)|
|专用端口|IUserRepository|[IUserRepository.h](../../../backend/application/ports/IUserRepository/IUserRepository.h)|
|专用端口|Ports|[Ports.h](../../../backend/application/ports/Ports/Ports.h)|
|内存驱动|MemoryAccountRepository|[MemoryAccountRepository.cpp](../../../backend/application/storage/memory/MemoryAccountRepository/MemoryAccountRepository.cpp)|
|内存驱动|MemoryAccountRepository|[MemoryAccountRepository.h](../../../backend/application/storage/memory/MemoryAccountRepository/MemoryAccountRepository.h)|
|内存驱动|MemoryContactsRepository|[MemoryContactsRepository.cpp](../../../backend/application/storage/memory/MemoryContactsRepository/MemoryContactsRepository.cpp)|
|内存驱动|MemoryContactsRepository|[MemoryContactsRepository.h](../../../backend/application/storage/memory/MemoryContactsRepository/MemoryContactsRepository.h)|
|内存驱动|MemoryConversationRepository|[MemoryConversationRepository.cpp](../../../backend/application/storage/memory/MemoryConversationRepository/MemoryConversationRepository.cpp)|
|内存驱动|MemoryConversationRepository|[MemoryConversationRepository.h](../../../backend/application/storage/memory/MemoryConversationRepository/MemoryConversationRepository.h)|
|内存驱动|MemoryEvaluationRepository|[MemoryEvaluationRepository.cpp](../../../backend/application/storage/memory/MemoryEvaluationRepository/MemoryEvaluationRepository.cpp)|
|内存驱动|MemoryEvaluationRepository|[MemoryEvaluationRepository.h](../../../backend/application/storage/memory/MemoryEvaluationRepository/MemoryEvaluationRepository.h)|
|内存驱动|MemoryFileRepository|[MemoryFileRepository.cpp](../../../backend/application/storage/memory/MemoryFileRepository/MemoryFileRepository.cpp)|
|内存驱动|MemoryFileRepository|[MemoryFileRepository.h](../../../backend/application/storage/memory/MemoryFileRepository/MemoryFileRepository.h)|
|内存驱动|MemoryGenerationAuditRepository|[MemoryGenerationAuditRepository.cpp](../../../backend/application/storage/memory/MemoryGenerationAuditRepository/MemoryGenerationAuditRepository.cpp)|
|内存驱动|MemoryGenerationAuditRepository|[MemoryGenerationAuditRepository.h](../../../backend/application/storage/memory/MemoryGenerationAuditRepository/MemoryGenerationAuditRepository.h)|
|内存驱动|MemoryKnowledgeRepository|[MemoryKnowledgeRepository.cpp](../../../backend/application/storage/memory/MemoryKnowledgeRepository/MemoryKnowledgeRepository.cpp)|
|内存驱动|MemoryKnowledgeRepository|[MemoryKnowledgeRepository.h](../../../backend/application/storage/memory/MemoryKnowledgeRepository/MemoryKnowledgeRepository.h)|
|内存驱动|MemoryMessageWriteRepository|[MemoryMessageWriteRepository.cpp](../../../backend/application/storage/memory/MemoryMessageWriteRepository/MemoryMessageWriteRepository.cpp)|
|内存驱动|MemoryMessageWriteRepository|[MemoryMessageWriteRepository.h](../../../backend/application/storage/memory/MemoryMessageWriteRepository/MemoryMessageWriteRepository.h)|
|内存驱动|MemoryModelRepository|[MemoryModelRepository.cpp](../../../backend/application/storage/memory/MemoryModelRepository/MemoryModelRepository.cpp)|
|内存驱动|MemoryModelRepository|[MemoryModelRepository.h](../../../backend/application/storage/memory/MemoryModelRepository/MemoryModelRepository.h)|
|内存驱动|MemoryOutboxRepository|[MemoryOutboxRepository.cpp](../../../backend/application/storage/memory/MemoryOutboxRepository/MemoryOutboxRepository.cpp)|
|内存驱动|MemoryOutboxRepository|[MemoryOutboxRepository.h](../../../backend/application/storage/memory/MemoryOutboxRepository/MemoryOutboxRepository.h)|
|内存驱动|MemoryState|[MemoryState.h](../../../backend/application/storage/memory/MemoryState/MemoryState.h)|
|内存驱动|MemoryTrainingRepository|[MemoryTrainingRepository.cpp](../../../backend/application/storage/memory/MemoryTrainingRepository/MemoryTrainingRepository.cpp)|
|内存驱动|MemoryTrainingRepository|[MemoryTrainingRepository.h](../../../backend/application/storage/memory/MemoryTrainingRepository/MemoryTrainingRepository.h)|
|内存驱动|MemoryWorkspaceRepository|[MemoryWorkspaceRepository.cpp](../../../backend/application/storage/memory/MemoryWorkspaceRepository/MemoryWorkspaceRepository.cpp)|
|内存驱动|MemoryWorkspaceRepository|[MemoryWorkspaceRepository.h](../../../backend/application/storage/memory/MemoryWorkspaceRepository/MemoryWorkspaceRepository.h)|
|PostgreSQL驱动|PostgresAccountRepository|[PostgresAccountRepository.cpp](../../../backend/application/storage/postgres/PostgresAccountRepository/PostgresAccountRepository.cpp)|
|PostgreSQL驱动|PostgresAccountRepository|[PostgresAccountRepository.h](../../../backend/application/storage/postgres/PostgresAccountRepository/PostgresAccountRepository.h)|
|PostgreSQL驱动|PostgresContactsRepository|[PostgresContactsRepository.cpp](../../../backend/application/storage/postgres/PostgresContactsRepository/PostgresContactsRepository.cpp)|
|PostgreSQL驱动|PostgresContactsRepository|[PostgresContactsRepository.h](../../../backend/application/storage/postgres/PostgresContactsRepository/PostgresContactsRepository.h)|
|PostgreSQL驱动|PostgresConversationRepository|[PostgresConversationRepository.cpp](../../../backend/application/storage/postgres/PostgresConversationRepository/PostgresConversationRepository.cpp)|
|PostgreSQL驱动|PostgresConversationRepository|[PostgresConversationRepository.h](../../../backend/application/storage/postgres/PostgresConversationRepository/PostgresConversationRepository.h)|
|PostgreSQL驱动|PostgresEvaluationRepository|[PostgresEvaluationRepository.cpp](../../../backend/application/storage/postgres/PostgresEvaluationRepository/PostgresEvaluationRepository.cpp)|
|PostgreSQL驱动|PostgresEvaluationRepository|[PostgresEvaluationRepository.h](../../../backend/application/storage/postgres/PostgresEvaluationRepository/PostgresEvaluationRepository.h)|
|PostgreSQL驱动|PostgresFileRepository|[PostgresFileRepository.cpp](../../../backend/application/storage/postgres/PostgresFileRepository/PostgresFileRepository.cpp)|
|PostgreSQL驱动|PostgresFileRepository|[PostgresFileRepository.h](../../../backend/application/storage/postgres/PostgresFileRepository/PostgresFileRepository.h)|
|PostgreSQL驱动|PostgresGenerationAuditRepository|[PostgresGenerationAuditRepository.cpp](../../../backend/application/storage/postgres/PostgresGenerationAuditRepository/PostgresGenerationAuditRepository.cpp)|
|PostgreSQL驱动|PostgresGenerationAuditRepository|[PostgresGenerationAuditRepository.h](../../../backend/application/storage/postgres/PostgresGenerationAuditRepository/PostgresGenerationAuditRepository.h)|
|PostgreSQL驱动|PostgresKnowledgeRepository|[PostgresKnowledgeRepository.cpp](../../../backend/application/storage/postgres/PostgresKnowledgeRepository/PostgresKnowledgeRepository.cpp)|
|PostgreSQL驱动|PostgresKnowledgeRepository|[PostgresKnowledgeRepository.h](../../../backend/application/storage/postgres/PostgresKnowledgeRepository/PostgresKnowledgeRepository.h)|
|PostgreSQL驱动|PostgresMessageRepository|[PostgresMessageRepository.cpp](../../../backend/application/storage/postgres/PostgresMessageRepository/PostgresMessageRepository.cpp)|
|PostgreSQL驱动|PostgresMessageRepository|[PostgresMessageRepository.h](../../../backend/application/storage/postgres/PostgresMessageRepository/PostgresMessageRepository.h)|
|PostgreSQL驱动|PostgresMessageWriteRepository|[PostgresMessageWriteRepository.cpp](../../../backend/application/storage/postgres/PostgresMessageWriteRepository/PostgresMessageWriteRepository.cpp)|
|PostgreSQL驱动|PostgresMessageWriteRepository|[PostgresMessageWriteRepository.h](../../../backend/application/storage/postgres/PostgresMessageWriteRepository/PostgresMessageWriteRepository.h)|
|PostgreSQL驱动|PostgresModelRepository|[PostgresModelRepository.cpp](../../../backend/application/storage/postgres/PostgresModelRepository/PostgresModelRepository.cpp)|
|PostgreSQL驱动|PostgresModelRepository|[PostgresModelRepository.h](../../../backend/application/storage/postgres/PostgresModelRepository/PostgresModelRepository.h)|
|PostgreSQL驱动|PostgresOutboxRepository|[PostgresOutboxRepository.cpp](../../../backend/application/storage/postgres/PostgresOutboxRepository/PostgresOutboxRepository.cpp)|
|PostgreSQL驱动|PostgresOutboxRepository|[PostgresOutboxRepository.h](../../../backend/application/storage/postgres/PostgresOutboxRepository/PostgresOutboxRepository.h)|
|PostgreSQL驱动|PostgresTrainingRepository|[PostgresTrainingRepository.cpp](../../../backend/application/storage/postgres/PostgresTrainingRepository/PostgresTrainingRepository.cpp)|
|PostgreSQL驱动|PostgresTrainingRepository|[PostgresTrainingRepository.h](../../../backend/application/storage/postgres/PostgresTrainingRepository/PostgresTrainingRepository.h)|
|PostgreSQL驱动|PostgresWorkspaceRepository|[PostgresWorkspaceRepository.cpp](../../../backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.cpp)|
|PostgreSQL驱动|PostgresWorkspaceRepository|[PostgresWorkspaceRepository.h](../../../backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.h)|
|HTTP适配|ContactsHttp|[ContactsHttp.cpp](../../../backend/application/transport/http/ContactsHttp/ContactsHttp.cpp)|
|HTTP适配|ContactsHttp|[ContactsHttp.h](../../../backend/application/transport/http/ContactsHttp/ContactsHttp.h)|
|HTTP适配|ConversationsHttp|[ConversationsHttp.cpp](../../../backend/application/transport/http/ConversationsHttp/ConversationsHttp.cpp)|
|HTTP适配|ConversationsHttp|[ConversationsHttp.h](../../../backend/application/transport/http/ConversationsHttp/ConversationsHttp.h)|
|HTTP适配|GroupGovernanceHttp|[GroupGovernanceHttp.cpp](../../../backend/application/transport/http/GroupGovernanceHttp/GroupGovernanceHttp.cpp)|
|HTTP适配|GroupGovernanceHttp|[GroupGovernanceHttp.h](../../../backend/application/transport/http/GroupGovernanceHttp/GroupGovernanceHttp.h)|
|HTTP适配|MessageHistoryHttp|[MessageHistoryHttp.cpp](../../../backend/application/transport/http/MessageHistoryHttp/MessageHistoryHttp.cpp)|
|HTTP适配|MessageHistoryHttp|[MessageHistoryHttp.h](../../../backend/application/transport/http/MessageHistoryHttp/MessageHistoryHttp.h)|
|HTTP适配|Phase11AuthHttp|[Phase11AuthHttp.cpp](../../../backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.cpp)|
|HTTP适配|Phase11AuthHttp|[Phase11AuthHttp.h](../../../backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h)|
|HTTP适配|Phase11FileHttp|[Phase11FileHttp.cpp](../../../backend/application/transport/http/Phase11FileHttp/Phase11FileHttp.cpp)|
|HTTP适配|Phase11FileHttp|[Phase11FileHttp.h](../../../backend/application/transport/http/Phase11FileHttp/Phase11FileHttp.h)|
|HTTP适配|Phase11KnowledgeHttp|[Phase11KnowledgeHttp.cpp](../../../backend/application/transport/http/Phase11KnowledgeHttp/Phase11KnowledgeHttp.cpp)|
|HTTP适配|Phase11KnowledgeHttp|[Phase11KnowledgeHttp.h](../../../backend/application/transport/http/Phase11KnowledgeHttp/Phase11KnowledgeHttp.h)|
|HTTP适配|Phase11SocialChatHttp|[Phase11SocialChatHttp.cpp](../../../backend/application/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.cpp)|
|HTTP适配|Phase11SocialChatHttp|[Phase11SocialChatHttp.h](../../../backend/application/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.h)|
|HTTP适配|Phase11TrainingHttp|[Phase11TrainingHttp.cpp](../../../backend/application/transport/http/Phase11TrainingHttp/Phase11TrainingHttp.cpp)|
|HTTP适配|Phase11TrainingHttp|[Phase11TrainingHttp.h](../../../backend/application/transport/http/Phase11TrainingHttp/Phase11TrainingHttp.h)|
|前端事件与依赖|AccountBindings|[AccountBindings.js](../../../frontend/phase11/chat/AccountBindings/AccountBindings.js)|
|前端事件与依赖|AiBindings|[AiBindings.js](../../../frontend/phase11/chat/AiBindings/AiBindings.js)|
|前端事件与依赖|ChatBindings|[ChatBindings.js](../../../frontend/phase11/chat/ChatBindings/ChatBindings.js)|
|前端事件与依赖|ComposerBindings|[ComposerBindings.js](../../../frontend/phase11/chat/ComposerBindings/ComposerBindings.js)|
|前端事件与依赖|ContactsBindings|[ContactsBindings.js](../../../frontend/phase11/chat/ContactsBindings/ContactsBindings.js)|
|前端事件与依赖|ConversationBindings|[ConversationBindings.js](../../../frontend/phase11/chat/ConversationBindings/ConversationBindings.js)|
|前端事件与依赖|DialogBindings|[DialogBindings.js](../../../frontend/phase11/chat/DialogBindings/DialogBindings.js)|
|前端事件与依赖|GroupBindings|[GroupBindings.js](../../../frontend/phase11/chat/GroupBindings/GroupBindings.js)|
|前端事件与依赖|KnowledgeBindings|[KnowledgeBindings.js](../../../frontend/phase11/chat/KnowledgeBindings/KnowledgeBindings.js)|
|装配/展示/验证|BusinessHttpExecutor|[BusinessHttpExecutor.h](../../../backend/application/transport/http/BusinessHttpExecutor/BusinessHttpExecutor.h)|
|装配/展示/验证|BusinessHttpCodec|[BusinessHttpCodec.h](../../../backend/application/transport/http/BusinessHttpCodec/BusinessHttpCodec.h)|
|装配/展示/验证|LearningRoutes|[LearningRoutes.cpp](../../../backend/application/transport/learning/LearningRoutes/LearningRoutes.cpp)|
|装配/展示/验证|ModelProviderAssembly|[ModelProviderAssembly.cpp](../../../backend/ai/bootstrap/ModelProviderAssembly/ModelProviderAssembly.cpp)|
|装配/展示/验证|BusinessMetricsFormatter|[BusinessMetricsFormatter.cpp](../../../backend/application/observability/BusinessMetricsFormatter/BusinessMetricsFormatter.cpp)|
|装配/展示/验证|PostgresValueCodec|[PostgresValueCodec.h](../../../backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h)|
|装配/展示/验证|ChatScope|[ChatScope.js](../../../frontend/phase11/chat/ChatScope/ChatScope.js)|
|装配/展示/验证|DesignSystem|[tokens.css](../../../frontend/phase11/shared/DesignSystem/tokens.css)|
|装配/展示/验证|project|[check_presentation.py](../../../scripts/project/check_presentation.py)|
|装配/展示/验证|frontend|[chat_scope.test.mjs](../../../tests/function3/frontend/chat_scope.test.mjs)|
|装配/展示/验证|frontend|[palette_assertions.cjs](../../../tests/function3/frontend/palette_assertions.cjs)|
|装配/展示/验证|project|[test_snapshot_pruning.py](../../../tests/project/test_snapshot_pruning.py)|

## 主要定位方法

- 提交唤醒观测：[CommitWakeClock.h](../../../backend/application/observability/CommitWakeClock/CommitWakeClock.h)、[commit_wakeup.cpp](../../../tests/function3/unit/commit_wakeup.cpp)。
- 真实指标出口：[BusinessMetrics.h](../../../backend/application/observability/BusinessMetrics/BusinessMetrics.h)、[Phase11Realtime.cpp](../../../backend/application/transport/realtime/Phase11Realtime/Phase11Realtime.cpp)。

- 账号、会话、模型等持久化：先看领域 Port，再看对应 Postgres/Memory Repository。
- 连接和事务：PostgresStore::Transaction / connection；MemoryStore 的锁和整体快照。
- 好友查询与群治理：ContactsHttp / GroupGovernanceHttp；业务判断仍交给 SocialService / ChatService。
- 页面事件：对应领域 Bindings；状态更新与动作交给 Controller，dependencies声明所需能力。
- 颜色：tokens.css中的语义角色；布局仍在组件CSS，禁止新增独立硬编码配色。
- 批量确认：IOutboxRepository → OutboxDispatcher → PostgresOutboxRepository / MemoryOutboxRepository。
