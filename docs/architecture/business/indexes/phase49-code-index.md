# Phase49 组件索引

按领域和职责定位；同名头文件/实现属于一个组件，不按按钮拆文件。旧头文件仅兼容转发。

## 进程与业务装配

|文件|索引|
|---|---|
|main.cpp|[main.cpp](../../../../main.cpp)|
|backend/application/bootstrap/ApplicationRuntime/ApplicationRuntime.h|[ApplicationRuntime.h](../../../../backend/application/bootstrap/ApplicationRuntime/ApplicationRuntime.h)|
|backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h|[BootstrapSettings.h](../../../../backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h)|
|backend/application/bootstrap/BusinessRuntime/BusinessRuntime.h|[BusinessRuntime.h](../../../../backend/application/bootstrap/BusinessRuntime/BusinessRuntime.h)|
|backend/application/bootstrap/ChatRuntime/ChatRuntime.h|[ChatRuntime.h](../../../../backend/application/bootstrap/ChatRuntime/ChatRuntime.h)|
|backend/application/bootstrap/ContactsRuntime/ContactsRuntime.h|[ContactsRuntime.h](../../../../backend/application/bootstrap/ContactsRuntime/ContactsRuntime.h)|
|backend/application/bootstrap/FileRuntime/FileRuntime.h|[FileRuntime.h](../../../../backend/application/bootstrap/FileRuntime/FileRuntime.h)|
|backend/application/bootstrap/GrpcRuntime/GrpcRuntime.h|[GrpcRuntime.h](../../../../backend/application/bootstrap/GrpcRuntime/GrpcRuntime.h)|
|backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h|[IdentityRuntime.h](../../../../backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h)|
|backend/application/bootstrap/NetworkSetup/NetworkSetup.h|[NetworkSetup.h](../../../../backend/application/bootstrap/NetworkSetup/NetworkSetup.h)|
|backend/application/bootstrap/OperationalRuntime/OperationalRuntime.h|[OperationalRuntime.h](../../../../backend/application/bootstrap/OperationalRuntime/OperationalRuntime.h)|
|backend/application/bootstrap/RealtimeRuntime/RealtimeRuntime.h|[RealtimeRuntime.h](../../../../backend/application/bootstrap/RealtimeRuntime/RealtimeRuntime.h)|
|backend/application/bootstrap/StorageRuntime/StorageRuntime.h|[StorageRuntime.h](../../../../backend/application/bootstrap/StorageRuntime/StorageRuntime.h)|
|backend/application/bootstrap/ApplicationRuntime/ApplicationRuntime.cpp|[ApplicationRuntime.cpp](../../../../backend/application/bootstrap/ApplicationRuntime/ApplicationRuntime.cpp)|
|backend/application/bootstrap/BusinessRuntime/BusinessRuntime.cpp|[BusinessRuntime.cpp](../../../../backend/application/bootstrap/BusinessRuntime/BusinessRuntime.cpp)|
|backend/application/bootstrap/ChatRuntime/ChatRuntime.cpp|[ChatRuntime.cpp](../../../../backend/application/bootstrap/ChatRuntime/ChatRuntime.cpp)|
|backend/application/bootstrap/ContactsRuntime/ContactsRuntime.cpp|[ContactsRuntime.cpp](../../../../backend/application/bootstrap/ContactsRuntime/ContactsRuntime.cpp)|
|backend/application/bootstrap/FileRuntime/FileRuntime.cpp|[FileRuntime.cpp](../../../../backend/application/bootstrap/FileRuntime/FileRuntime.cpp)|
|backend/application/bootstrap/GrpcRuntime/GrpcRuntime.cpp|[GrpcRuntime.cpp](../../../../backend/application/bootstrap/GrpcRuntime/GrpcRuntime.cpp)|
|backend/application/bootstrap/IdentityRuntime/IdentityRuntime.cpp|[IdentityRuntime.cpp](../../../../backend/application/bootstrap/IdentityRuntime/IdentityRuntime.cpp)|
|backend/application/bootstrap/NetworkSetup/NetworkSetup.cpp|[NetworkSetup.cpp](../../../../backend/application/bootstrap/NetworkSetup/NetworkSetup.cpp)|
|backend/application/bootstrap/OperationalRuntime/OperationalRuntime.cpp|[OperationalRuntime.cpp](../../../../backend/application/bootstrap/OperationalRuntime/OperationalRuntime.cpp)|
|backend/application/bootstrap/RealtimeRuntime/RealtimeRuntime.cpp|[RealtimeRuntime.cpp](../../../../backend/application/bootstrap/RealtimeRuntime/RealtimeRuntime.cpp)|
|backend/application/bootstrap/StorageRuntime/StorageRuntime.cpp|[StorageRuntime.cpp](../../../../backend/application/bootstrap/StorageRuntime/StorageRuntime.cpp)|

## AI 装配

|文件|索引|
|---|---|
|backend/ai/bootstrap/AiRuntime/AiRuntime.cpp|[AiRuntime.cpp](../../../../backend/ai/bootstrap/AiRuntime/AiRuntime.cpp)|
|backend/ai/bootstrap/AiRuntime/AiRuntime.h|[AiRuntime.h](../../../../backend/ai/bootstrap/AiRuntime/AiRuntime.h)|
|backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.cpp|[KnowledgeRuntime.cpp](../../../../backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.cpp)|
|backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.h|[KnowledgeRuntime.h](../../../../backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.h)|

## 领域服务与协议入口

|文件|索引|
|---|---|
|backend/application/modules/accounts/application/AuthService/AuthService.cpp|[AuthService.cpp](../../../../backend/application/modules/accounts/application/AuthService/AuthService.cpp)|
|backend/application/modules/accounts/application/AuthService/AuthService.h|[AuthService.h](../../../../backend/application/modules/accounts/application/AuthService/AuthService.h)|
|backend/application/modules/contacts/application/SocialService/SocialService.cpp|[SocialService.cpp](../../../../backend/application/modules/contacts/application/SocialService/SocialService.cpp)|
|backend/application/modules/contacts/application/SocialService/SocialService.h|[SocialService.h](../../../../backend/application/modules/contacts/application/SocialService/SocialService.h)|
|backend/application/modules/contacts/application/UserDirectoryService/UserDirectoryService.cpp|[UserDirectoryService.cpp](../../../../backend/application/modules/contacts/application/UserDirectoryService/UserDirectoryService.cpp)|
|backend/application/modules/contacts/application/UserDirectoryService/UserDirectoryService.h|[UserDirectoryService.h](../../../../backend/application/modules/contacts/application/UserDirectoryService/UserDirectoryService.h)|
|backend/application/modules/contacts/domain/ContactDirectoryTypes/ContactDirectoryTypes.h|[ContactDirectoryTypes.h](../../../../backend/application/modules/contacts/domain/ContactDirectoryTypes/ContactDirectoryTypes.h)|
|backend/application/modules/contacts/ports/IUserDirectoryRepository/IUserDirectoryRepository.h|[IUserDirectoryRepository.h](../../../../backend/application/modules/contacts/ports/IUserDirectoryRepository/IUserDirectoryRepository.h)|
|backend/application/modules/contacts/transport/ContactsHttp/ContactsHttp.cpp|[ContactsHttp.cpp](../../../../backend/application/modules/contacts/transport/ContactsHttp/ContactsHttp.cpp)|
|backend/application/modules/contacts/transport/ContactsHttp/ContactsHttp.h|[ContactsHttp.h](../../../../backend/application/modules/contacts/transport/ContactsHttp/ContactsHttp.h)|
|backend/application/modules/contacts/transport/UserDirectoryHttp/UserDirectoryHttp.cpp|[UserDirectoryHttp.cpp](../../../../backend/application/modules/contacts/transport/UserDirectoryHttp/UserDirectoryHttp.cpp)|
|backend/application/modules/contacts/transport/UserDirectoryHttp/UserDirectoryHttp.h|[UserDirectoryHttp.h](../../../../backend/application/modules/contacts/transport/UserDirectoryHttp/UserDirectoryHttp.h)|
|backend/application/modules/chat/application/ChatService/ChatService.cpp|[ChatService.cpp](../../../../backend/application/modules/chat/application/ChatService/ChatService.cpp)|
|backend/application/modules/chat/application/ChatService/ChatService.h|[ChatService.h](../../../../backend/application/modules/chat/application/ChatService/ChatService.h)|
|backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.cpp|[ChatWorkspaceService.cpp](../../../../backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.cpp)|
|backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.h|[ChatWorkspaceService.h](../../../../backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.h)|
|backend/application/modules/chat/application/WorkspaceRequests/WorkspaceRequests.h|[WorkspaceRequests.h](../../../../backend/application/modules/chat/application/WorkspaceRequests/WorkspaceRequests.h)|
|backend/application/modules/chat/bootstrap/ChatWorkspaceModule/ChatWorkspaceModule.h|[ChatWorkspaceModule.h](../../../../backend/application/modules/chat/bootstrap/ChatWorkspaceModule/ChatWorkspaceModule.h)|
|backend/application/modules/chat/domain/WorkspaceTypes/WorkspaceTypes.h|[WorkspaceTypes.h](../../../../backend/application/modules/chat/domain/WorkspaceTypes/WorkspaceTypes.h)|
|backend/application/modules/chat/ports/IWorkspaceRepository/IWorkspaceRepository.h|[IWorkspaceRepository.h](../../../../backend/application/modules/chat/ports/IWorkspaceRepository/IWorkspaceRepository.h)|
|backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.cpp|[ChatWorkspaceHttp.cpp](../../../../backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.cpp)|
|backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.h|[ChatWorkspaceHttp.h](../../../../backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.h)|
|backend/application/modules/chat/transport/ConversationsHttp/ConversationsHttp.cpp|[ConversationsHttp.cpp](../../../../backend/application/modules/chat/transport/ConversationsHttp/ConversationsHttp.cpp)|
|backend/application/modules/chat/transport/ConversationsHttp/ConversationsHttp.h|[ConversationsHttp.h](../../../../backend/application/modules/chat/transport/ConversationsHttp/ConversationsHttp.h)|
|backend/application/modules/chat/transport/GroupGovernanceHttp/GroupGovernanceHttp.cpp|[GroupGovernanceHttp.cpp](../../../../backend/application/modules/chat/transport/GroupGovernanceHttp/GroupGovernanceHttp.cpp)|
|backend/application/modules/chat/transport/GroupGovernanceHttp/GroupGovernanceHttp.h|[GroupGovernanceHttp.h](../../../../backend/application/modules/chat/transport/GroupGovernanceHttp/GroupGovernanceHttp.h)|
|backend/application/modules/chat/transport/MessageHistoryHttp/MessageHistoryHttp.cpp|[MessageHistoryHttp.cpp](../../../../backend/application/modules/chat/transport/MessageHistoryHttp/MessageHistoryHttp.cpp)|
|backend/application/modules/chat/transport/MessageHistoryHttp/MessageHistoryHttp.h|[MessageHistoryHttp.h](../../../../backend/application/modules/chat/transport/MessageHistoryHttp/MessageHistoryHttp.h)|
|backend/application/modules/files/application/FileService/FileService.cpp|[FileService.cpp](../../../../backend/application/modules/files/application/FileService/FileService.cpp)|
|backend/application/modules/files/application/FileService/FileService.h|[FileService.h](../../../../backend/application/modules/files/application/FileService/FileService.h)|

## 数据库映射

|文件|索引|
|---|---|
|backend/application/storage/postgres/PostgresWorkspaceRepository/WorkspaceRows.h|[WorkspaceRows.h](../../../../backend/application/storage/postgres/PostgresWorkspaceRepository/WorkspaceRows.h)|
|backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.cpp|[PostgresWorkspaceRepository.cpp](../../../../backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.cpp)|
|backend/application/storage/postgres/PostgresStore/PostgresStore.cpp|[PostgresStore.cpp](../../../../backend/application/storage/postgres/PostgresStore/PostgresStore.cpp)|

## 前端领域入口

|文件|索引|
|---|---|
|frontend/phase11/accounts/AccountBindings/AccountBindings.js|[AccountBindings.js](../../../../frontend/phase11/accounts/AccountBindings/AccountBindings.js)|
|frontend/phase11/accounts/AccountsApi/AccountsApi.js|[AccountsApi.js](../../../../frontend/phase11/accounts/AccountsApi/AccountsApi.js)|
|frontend/phase11/contacts/ContactsApi/ContactsApi.js|[ContactsApi.js](../../../../frontend/phase11/contacts/ContactsApi/ContactsApi.js)|
|frontend/phase11/contacts/ContactsBindings/ContactsBindings.js|[ContactsBindings.js](../../../../frontend/phase11/contacts/ContactsBindings/ContactsBindings.js)|
|frontend/phase11/contacts/ContactsController/ContactsController.js|[ContactsController.js](../../../../frontend/phase11/contacts/ContactsController/ContactsController.js)|
|frontend/phase11/ai/AiApi/AiApi.js|[AiApi.js](../../../../frontend/phase11/ai/AiApi/AiApi.js)|
|frontend/phase11/ai/AiBindings/AiBindings.js|[AiBindings.js](../../../../frontend/phase11/ai/AiBindings/AiBindings.js)|
|frontend/phase11/ai/AiController/AiController.js|[AiController.js](../../../../frontend/phase11/ai/AiController/AiController.js)|
|frontend/phase11/ai/PrivateAiApi/PrivateAiApi.js|[PrivateAiApi.js](../../../../frontend/phase11/ai/PrivateAiApi/PrivateAiApi.js)|
|frontend/phase11/ai/WebAiApi/WebAiApi.js|[WebAiApi.js](../../../../frontend/phase11/ai/WebAiApi/WebAiApi.js)|
|frontend/phase11/files/AttachmentController/AttachmentController.js|[AttachmentController.js](../../../../frontend/phase11/files/AttachmentController/AttachmentController.js)|
|frontend/phase11/files/FilesApi/FilesApi.js|[FilesApi.js](../../../../frontend/phase11/files/FilesApi/FilesApi.js)|
|frontend/phase11/knowledge/KnowledgeApi/KnowledgeApi.js|[KnowledgeApi.js](../../../../frontend/phase11/knowledge/KnowledgeApi/KnowledgeApi.js)|
|frontend/phase11/knowledge/KnowledgeBindings/KnowledgeBindings.js|[KnowledgeBindings.js](../../../../frontend/phase11/knowledge/KnowledgeBindings/KnowledgeBindings.js)|
|frontend/phase11/knowledge/KnowledgeController/KnowledgeController.js|[KnowledgeController.js](../../../../frontend/phase11/knowledge/KnowledgeController/KnowledgeController.js)|

## 契约与验证

|文件|索引|
|---|---|
|scripts/project/sync_api_contracts.py|[sync_api_contracts.py](../../../../scripts/project/sync_api_contracts.py)|
|docs/contracts/business-routes.json|[business-routes.json](../../../contracts/business-routes.json)|
|frontend/phase11/shared/ApiContracts/ApiContracts.js|[ApiContracts.js](../../../../frontend/phase11/shared/ApiContracts/ApiContracts.js)|
|frontend/phase11/shared/ApiContracts/ApiCatalog.js|[ApiCatalog.js](../../../../frontend/phase11/shared/ApiContracts/ApiCatalog.js)|
|frontend/phase11/shared/BusinessApi/BusinessApi.js|[BusinessApi.js](../../../../frontend/phase11/shared/BusinessApi/BusinessApi.js)|
|scripts/project/check_boundaries.py|[check_boundaries.py](../../../../scripts/project/check_boundaries.py)|
|tests/function3/frontend/api_contracts.test.mjs|[api_contracts.test.mjs](../../../../tests/function3/frontend/api_contracts.test.mjs)|
|tests/project/test_api_contracts.py|[test_api_contracts.py](../../../../tests/project/test_api_contracts.py)|
|tests/function3/unit/WorkspaceContract.h|[WorkspaceContract.h](../../../../tests/function3/unit/WorkspaceContract.h)|
|tests/function3/integration/assembly_lifecycle_blackbox.py|[assembly_lifecycle_blackbox.py](../../../../tests/function3/integration/assembly_lifecycle_blackbox.py)|

其他页面的 XxxApi 绑定位于各自领域目录；完整 119 项 HTTP 方法/路径与后端注册文件见 [路由目录](../../../contracts/business-routes.json)。WS/SSE 的行为约定继续见 [实时契约](../../../contracts/realtime-contracts.json)。
