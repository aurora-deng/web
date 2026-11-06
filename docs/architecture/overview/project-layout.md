# 项目目录与构建边界

目录以 `Function3.2` 为基准。下图展开源码主干，并重点展开社区装配、事件投递和复现入口；聊天 ACK 采样位于 `backend/application/observability/ChatAckTrace/`，隔离演练位于 `scripts/function3/validation/function32_lab.py`。构建产物与本地数据见下文。

```text
项目根目录
├─ main.cpp                          Linux 进程入口
├─ CMakeLists.txt / CMakePresets.json  Linux 构建目标与预设
├─ Dockerfile                         通信演示镜像草案
├─ .github/workflows/                 源码检查与手动触发的业务验证
├─ backend/
│  ├─ server/                         Linux 通信核心
│  │  ├─ Reactor/、SubReactor/、Runtime/、session/
│  │  ├─ http/、http2/、websocket/、sse/、tls/、grpc/
│  │  └─ Route/、Executor/、CoroutineScheduler/ 等
│  ├─ application/                    共享业务和运行时装配
│  │  ├─ bootstrap/                   应用、业务、身份、聊天与实时入口
│  │  ├─ modules/
│  │  │  ├─ bootstrap/
│  │  │  │  ├─ Function3Runtime/         社区总装配
│  │  │  │  └─ CommunityContentRuntime/  神树、动态、举报、媒体
│  │  │  ├─ messaging/
│  │  │  │  ├─ bootstrap/
│  │  │  │  │  └─ CommunityDeliveryRuntime/  Outbox、Broker、重放与通知
│  │  │  │  └─ application/、ports/、infrastructure/、transport/
│  │  │  ├─ accounts/、contacts/、chat/、files/
│  │  │  ├─ tree/、activities/、profiles/、moderation/
│  │  │  └─ admin/、ranking/、announcement/、assistant/、cache/、shared/
│  │  ├─ business/、domain/、ports/    既有业务、领域对象与接口
│  │  ├─ storage/
│  │  │  ├─ postgres/
│  │  │  │  └─ migrations/             编号迁移，当前要求 001–024
│  │  │  └─ memory/、InMemoryStore/、files/
│  │  ├─ transport/                   HTTP、WS、SSE 业务适配
│  │  └─ config/、observability/、runtime/、security/
│  └─ ai/
│     ├─ bootstrap/、models/          AI 装配、Provider、模型治理与 RAG
│     └─ private/、speech/、training/  私人生成、语音、训练与转换
├─ client/                            Windows 客户端与独立 CMake 入口
│  └─ companion/、assistant/、storage/  本机代理、私人助手、SQLite 与 DPAPI
├─ frontend/
│  └─ phase11/
│     ├─ accounts/、contacts/、chat/、files/
│     ├─ tree/、activities/、profile/、announcements/、admin/
│     ├─ ai/、assistant/、knowledge/、speech/、training/
│     └─ shared/、status/、vendor/
├─ static/                            其他静态资源
├─ scripts/
│  ├─ project/
│  │  ├─ reproduce.py                 Linux 通信档和业务档复现入口
│  │  ├─ repro_runtime.py             临时数据库、进程与业务冒烟
│  │  └─ ci_checks.py、check_doc_links.py 等
│  ├─ function3/                      日常运行、隔离验证、依赖准备
│  └─ phase11/、client/                既有脚本与 Windows 构建
├─ tests/                             单元、集成、Fuzz、工程与客户端测试
├─ benchmarks/                        压测工具和配置
├─ examples/                          WebSocket 可靠消费等教学示例
├─ tools/                             管理、评测、数据与模型工具
├─ artifacts/                         版本化证据及工作区本地证据
└─ docs/
   ├─ architecture/                   总览、底层通信、业务与源码索引
   ├─ database/、contracts/            数据库规则与接口契约
   ├─ ops/、testing/、performance/     运行、复现、测试与压测
   └─ reports/、evidence/、releases/、roadmap/、learning/、archive/
```

## 调用与构建边界

1. `main.cpp` 进入 `ApplicationRuntime`。协议适配把请求交给应用服务，服务依赖领域对象与 Port。PostgreSQL、Redis、RocketMQ 和云语音适配实现这些接口。
2. 社区装配由 `Function3Runtime` 接线。内容模块写入业务数据及同事务事件，投递模块在提交后处理 Outbox。CMake 目标分别为 `function3_community_content` 和 `function3_community_delivery`，由 `function3_http` 在 PostgreSQL 配置下装配。[社区模块说明](../business/engineering/community-runtime-modules.md)
3. Windows 使用 `client/CMakeLists.txt`。浏览器前端位于 `frontend/phase11/`。ONNX gRPC 推理、Kokoro 语音和显式训练或转换任务有各自的进程边界。
4. Linux 两档复现通过同一脚本进入。通信档验证 HTTP、静态资源和未配置业务时的返回，业务档使用一次性 PostgreSQL 跑 CTest 和真实业务冒烟。[复现指南](../../ops/validation/reproduce.md)

## 文件与数据位置

C++ 组件用同名目录存放头文件与实现。纯接口保留头文件，配套辅助代码随组件保存。源码树上方未展开 `build-*` 等本机构建目录。数据库、上传文件、私钥、云 Key、模型和私人训练资料按各自运行配置存放。

共享业务以 Linux PostgreSQL 为权威存储。Windows SQLite 存放按账号隔离的私人资料。数据库升级只追加编号迁移，原库变更和日常服务切换按运维文档操作。[迁移规则](../../database/migration-policy.md) · [日常运行](../../ops/runtime/function3-runtime.md)

[文档导航](../../README.md) · [当前端到端图](../business/flows/current-end-to-end.md) · [代码索引](../business/indexes/README.md) · [历史目录迁移清单](../source-layout-migration.json)
