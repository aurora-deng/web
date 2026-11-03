# Phase43–47组件与改动索引

路径相对项目根目录；纯端口/头文件组件不创建空cpp。以下链接均按实际文件生成。

|区域|职责/组件|文件|
|---|---|---|
|后端|统一既有配置和跨字段启动校验|[RuntimeConfig.cpp](../../../../backend/application/config/RuntimeConfig/RuntimeConfig.cpp)|
|后端|统一既有配置和跨字段启动校验|[RuntimeConfig.h](../../../../backend/application/config/RuntimeConfig/RuntimeConfig.h)|
|后端|数据库/排队/事件年龄指标|[BusinessMetrics.h](../../../../backend/application/observability/BusinessMetrics/BusinessMetrics.h)|
|后端|明确、幂等的业务停止顺序|[BusinessShutdown.h](../../../../backend/application/runtime/BusinessShutdown/BusinessShutdown.h)|
|后端|历史/搜索/定位的批量正文与附件装配|[PostgresMessageRepository.cpp](../../../../backend/application/storage/postgres/PostgresMessageRepository/PostgresMessageRepository.cpp)|
|后端|历史/搜索/定位的批量正文与附件装配|[PostgresMessageRepository.h](../../../../backend/application/storage/postgres/PostgresMessageRepository/PostgresMessageRepository.h)|
|后端|用户目录、可见消息搜索、版本化偏好SQL|[PostgresWorkspaceRepository.cpp](../../../../backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.cpp)|
|后端|用户目录、可见消息搜索、版本化偏好SQL|[PostgresWorkspaceRepository.h](../../../../backend/application/storage/postgres/PostgresWorkspaceRepository/PostgresWorkspaceRepository.h)|
|后端|PG结果RAII、参数化查询和LIKE转义|[PostgresSql.h](../../../../backend/application/storage/postgres/PostgresSql/PostgresSql.h)|
|后端|公开目录、游标、偏好、上下文值对象|[WorkspaceTypes.h](../../../../backend/application/modules/chat/domain/WorkspaceTypes/WorkspaceTypes.h)|
|后端|数据库无关聊天工作区端口|[IWorkspaceRepository.h](../../../../backend/application/modules/chat/ports/IWorkspaceRepository/IWorkspaceRepository.h)|
|后端|账号/成员规则、分页、上下文和偏好事务|[ChatWorkspaceService.cpp](../../../../backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.cpp)|
|后端|账号/成员规则、分页、上下文和偏好事务|[ChatWorkspaceService.h](../../../../backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.h)|
|后端|HTTP DTO/认证/执行器适配|[ChatWorkspaceHttp.cpp](../../../../backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.cpp)|
|后端|HTTP DTO/认证/执行器适配|[ChatWorkspaceHttp.h](../../../../backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.h)|
|后端|聊天领域依赖装配|[ChatWorkspaceModule.h](../../../../backend/application/modules/chat/bootstrap/ChatWorkspaceModule/ChatWorkspaceModule.h)|
|后端|实际执行与公开能力共用树媒体策略|[TreeContentPolicy.h](../../../../backend/application/modules/tree/domain/TreeContentPolicy/TreeContentPolicy.h)|
|前端聊天|AiController|[AiController.js](../../../../frontend/phase11/ai/AiController/AiController.js)|
|前端聊天|AttachmentController|[AttachmentController.js](../../../../frontend/phase11/files/AttachmentController/AttachmentController.js)|
|前端聊天|ChatApplication|[ChatApplication.js](../../../../frontend/phase11/chat/ChatApplication/ChatApplication.js)|
|前端聊天|ChatBindings|[ChatBindings.js](../../../../frontend/phase11/chat/ChatBindings/ChatBindings.js)|
|前端聊天|ChatContext|[ChatContext.js](../../../../frontend/phase11/chat/ChatContext/ChatContext.js)|
|前端聊天|ContactsController|[ContactsController.js](../../../../frontend/phase11/contacts/ContactsController/ContactsController.js)|
|前端聊天|ConversationController|[ConversationController.js](../../../../frontend/phase11/chat/ConversationController/ConversationController.js)|
|前端聊天|FeedbackController|[FeedbackController.js](../../../../frontend/phase11/chat/FeedbackController/FeedbackController.js)|
|前端聊天|GroupController|[GroupController.js](../../../../frontend/phase11/chat/GroupController/GroupController.js)|
|前端聊天|KnowledgeController|[KnowledgeController.js](../../../../frontend/phase11/knowledge/KnowledgeController/KnowledgeController.js)|
|前端聊天|MessageController|[MessageController.js](../../../../frontend/phase11/chat/MessageController/MessageController.js)|
|前端聊天|MessageGrouping|[MessageGrouping.js](../../../../frontend/phase11/chat/MessageGrouping/MessageGrouping.js)|
|前端聊天|PreferenceSnapshots|[PreferenceSnapshots.js](../../../../frontend/phase11/chat/PreferenceSnapshots/PreferenceSnapshots.js)|
|前端聊天|RealtimeController|[RealtimeController.js](../../../../frontend/phase11/chat/RealtimeController/RealtimeController.js)|
|前端聊天|UnreadWatermarks|[UnreadWatermarks.js](../../../../frontend/phase11/chat/UnreadWatermarks/UnreadWatermarks.js)|
|前端聊天|UserPicker|[UserPicker.js](../../../../frontend/phase11/chat/UserPicker/UserPicker.js)|
|前端聊天|WorkspaceUi|[WorkspaceUi.css](../../../../frontend/phase11/chat/WorkspaceUi/WorkspaceUi.css)|
|资源/工程/测试|TreeViewport|[TreeViewport.css](../../../../frontend/phase11/tree/TreeViewport/TreeViewport.css)|
|资源/工程/测试|UploadCapabilities|[UploadCapabilities.js](../../../../frontend/phase11/shared/UploadCapabilities/UploadCapabilities.js)|
|资源/工程/测试|index|[index.html](../../../../frontend/phase11/status/index.html)|
|资源/工程/测试|status|[status.js](../../../../frontend/phase11/status/status.js)|
|资源/工程/测试|verification|[verification.js](../../../../frontend/phase11/status/verification.js)|
|资源/工程/测试|source_guard|[source_guard.py](../../../../scripts/project/source_guard.py)|
|资源/工程/测试|check_boundaries|[check_boundaries.py](../../../../scripts/project/check_boundaries.py)|
|资源/工程/测试|ci_checks|[ci_checks.py](../../../../scripts/project/ci_checks.py)|
|资源/工程/测试|frontend_release|[frontend_release.py](../../../../scripts/project/frontend_release.py)|
|资源/工程/测试|build_stamp|[build_stamp.py](../../../../scripts/project/build_stamp.py)|
|资源/工程/测试|process_identity|[process_identity.py](../../../../scripts/function3/validation/process_identity.py)|
|资源/工程/测试|phase47_benchmark|[phase47_benchmark.py](../../../../scripts/function3/validation/phase47_benchmark.py)|
|资源/工程/测试|WorkspaceContract|[WorkspaceContract.h](../../../../tests/function3/unit/WorkspaceContract.h)|
|资源/工程/测试|workspace|[workspace.cpp](../../../../tests/function3/unit/workspace.cpp)|
|资源/工程/测试|runtime_config|[runtime_config.cpp](../../../../tests/function3/unit/runtime_config.cpp)|
|资源/工程/测试|business_shutdown|[business_shutdown.cpp](../../../../tests/function3/unit/business_shutdown.cpp)|
|资源/工程/测试|workspace_blackbox|[workspace_blackbox.py](../../../../tests/function3/integration/workspace_blackbox.py)|
|资源/工程/测试|unread_watermarks.test|[unread_watermarks.test.mjs](../../../../tests/function3/frontend/unread_watermarks.test.mjs)|
|资源/工程/测试|workspace_logic.test|[workspace_logic.test.mjs](../../../../tests/function3/frontend/workspace_logic.test.mjs)|
|资源/工程/测试|chat_workspace_browser|[chat_workspace_browser.cjs](../../../../tests/function3/frontend/chat_workspace_browser.cjs)|
|资源/工程/测试|test_ci_gates|[test_ci_gates.py](../../../../tests/project/test_ci_gates.py)|
|资源/工程/测试|test_process_identity|[test_process_identity.py](../../../../tests/project/test_process_identity.py)|
|资源/工程/测试|test_source_inventory|[test_source_inventory.py](../../../../tests/project/test_source_inventory.py)|
|Windows诊断|proxy_overhead|[proxy_overhead.py](../../../../tests/client/proxy_overhead.py)|
|Windows诊断|tls_probe|[tls_probe.cpp](../../../../tests/client/tls_probe.cpp)|

## 保留兼容入口的已有文件

|文件|本轮变化|
|---|---|
|`main.cpp`|配置委托、领域装配、版本端点、显式停止顺序|
|`CMakeLists.txt`|独立聊天用例目标、实际新增测试、构建指纹|
|`PostgresStore.{h,cpp}`|委托领域驱动、摘要字段、全迁移启动检查|
|`InMemoryStore.{h,cpp}`|同一工作区端口/契约、事务快照中的偏好|
|`Domain.h`|兼容摘要追加peer与偏好版本|
|`Phase11SocialChatHttp.cpp`|摘要追加精确字符串水位/偏好版本|
|`Phase11Realtime.cpp`|操作者偏好事件投递及事件年龄观察|
|`BoundedExecutor.cpp` / `SqlContext.cpp`|业务排队与SQL观测|
|`Function3Runtime.{h,cpp}`|完整迁移校验和显式幂等停止|
|`FrontendAssets.cpp`|仅将公开资源版本JSON加入白名单|
|`FileService.{h,cpp}` / `TreeService.cpp` / `TreeMediaService.cpp` / `FfmpegMediaProcessor.cpp`|同一上传类型/大小策略用于执行与公开说明|
|`BusinessApi.js` / `ActivityPublisher.js` / `MediaUploader.js`|请求失败语义、服务端能力和发布上传|
|`ChatHistory.js` / `VirtualMessageList.js`|有界上下文窗口、虚拟窗口首行分组反馈|
|`app.js` / `index.html` / tree入口|模块启动、新交互入口、视口样式|
|`.github/workflows/ci.yml` / `.env.example`|显式CI能力范围及配置说明|

统一前端版本会改动模块引用的`?v=`，这类变化不代表重新实现对应业务。`backend/server/`全部152份文件未修改。
