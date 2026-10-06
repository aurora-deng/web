# 项目目录分层

本次整理的目标不是把文件机械地塞进更多文件夹，而是让“看到路径就知道职责”。可以把仓库想成一座教学楼：
`server/` 是上课和运行的教室，`frontend/` 是用户入口，`tests/` 是考场，`scripts/` 是实验工具，
`docs/` 是教材，`artifacts/` 是已经产生的实验记录。运行代码、测试输入和测试结果不再混在同一层。

## 1. 当前目录

```text
web/
├─ CMakeLists.txt                  构建入口
├─ main.cpp                       进程装配入口，只连接组件和注册路由
├─ frontend/
│  └─ phase11/                    Phase 11 浏览器端 HTML/CSS/JavaScript
├─ server/
│  ├─ phase11/
│  │  ├─ ai/<Component>/          每个模型 Provider、Decoder、Worker 独立成组
│  │  ├─ business/<Component>/    业务接口、账号、社交、聊天、AI 和 Outbox 实现
│  │  ├─ domain/Domain/           领域实体和值对象
│  │  ├─ ports/Ports/             Repository、事务和安全端口
│  │  ├─ runtime/<Component>/     Phase 11 有界执行器
│  │  ├─ security/<Component>/    限流与凭据实现
│  │  ├─ storage/<Component>/     内存与 PostgreSQL 存储适配器
│  │  ├─ training/<Component>/    训练数据授权与导出
│  │  └─ transport/               HTTP、WebSocket、SSE DTO 与协议适配器
│  ├─ http/ websocket/ sse/       HTTP/1.1、WebSocket、SSE 协议层
│  ├─ http2/ grpc/ tls/           HTTP/2、gRPC、TLS/ALPN
│  ├─ Reactor/ SubReactor/        网络事件循环
│  ├─ CoroutineScheduler/         协程调度
│  ├─ Executor/ Runtime/          业务执行与进程生命周期
│  ├─ transport/                  跨协议出站队列和写入边界
│  └─ observability/logger/       日志与观测基础设施
├─ tests/
│  ├─ phase11/unit/               Phase 11 快速单元和 Repository 契约测试
│  ├─ phase11/integration/        真实进程、数据库、WS/SSE/AI 黑盒脚本
│  ├─ phase11/smoke/              真实 ONNX 模型最小加载验证
│  ├─ integration/                通用协议黑盒测试
│  └─ fuzz/                       解析器模糊测试源码与 corpus
├─ scripts/
│  └─ phase11/                    Phase 11 迁移、模型和运行辅助脚本
├─ docs/
│  ├─ architecture/               当前架构、阶段设计和版本差异
│  ├─ database/                   数据库表结构、关系与数据流学习文档
│  ├─ testing/ performance/ ops/  测试、性能与运维说明
│  ├─ reports/                    已完成的历史测试报告
│  └─ archive/                    仍有参考价值但不参与构建的旧代码
├─ artifacts/
│  ├─ benchmarks/                 已生成的基准结果
│  ├─ profiling/                  火焰图等性能分析产物
│  └─ test-results/               已生成的测试输出
├─ benchmarks/                    可重复执行的基准程序源码
├─ examples/                      面向学习的最小示例
└─ static/                        `/logo` 等通用静态响应测试资源
```

## 2. 本次移动

| 原位置 | 新位置 | 原因 |
|---|---|---|
| `static/phase11/` | `frontend/phase11/` | 与 `/logo` 等服务器测试资源分离，明确它是完整前端应用 |
| `tests/phase11_*.cpp` | `tests/phase11/unit/` | Phase 11 测试按层级集中 |
| `tests/integration/phase11_*` | `tests/phase11/integration/` | Phase 11 黑盒脚本与通用协议黑盒分开 |
| `fuzz/*.cpp`、`fuzz-corpus/` | `tests/fuzz/` | 模糊测试源码和输入都属于测试域 |
| `log/logger/` | `server/observability/logger/` | Logger 是服务端观测基础设施 |
| `benchmark-results/` | `artifacts/benchmarks/` | 结果与 `benchmarks/` 下的基准源码分开 |
| `test-output/` | `artifacts/test-results/` | 生成结果不占据仓库根目录 |
| `flame.svg` | `artifacts/profiling/flame.svg` | 性能分析产物集中保存 |
| `preview.cpp` | `docs/archive/legacy_subreactor_preview.cpp` | 历史教学快照不再伪装成当前构建源码 |
| 根目录历史测试讲义 | `docs/testing/learning/legacy-guide/` | 所有教材统一从文档导航进入 |
| `server/phase11/application/*` | `server/phase11/business/<Component>/` | 业务接口和业务实现归入同一层，传输层只依赖业务门面 |
| `server/phase11` 各分类目录中的平铺文件 | 对应 `<Component>/` 子目录 | 与 HTTP/WebSocket 一致，一个组件的 `.h/.cpp` 相邻存放 |
| 7 个空占位目录 | 删除 | 没有源码、构建引用或文档职责，保留只会干扰目录阅读 |

所有受影响的 CMake 源文件路径、`#include`、测试脚本和文档链接同步更新。文件只做了可追踪移动，
没有删除仍被构建、路由或测试引用的资源。

## 3. 前端资源如何进入运行目录

服务器的静态路由使用相对路径读取 `frontend/phase11`。过去从源码根目录启动时正常，从 build
目录启动则可能只得到 HTML 或 404，表现就是页面退回浏览器默认样式。现在 CMake 的
`phase11_frontend_assets` 目标会在每次构建时把资源同步到：

```text
<build>/frontend/phase11/
```

因此下面两种学习方式都能工作：

```bash
# 在源码根目录运行
./build/webserver

# 在构建目录运行
cd build && ./webserver
```

页面仍从同源 `/phase11/` 加载资源并调用 `/api/*`、`/ws`、`/events`，不引入额外开发服务器，
也不存在跨域配置差异。

## 4. 后续放置规则

1. 新协议实现进入 `server/<protocol>/`，跨协议共享发送逻辑进入 `server/transport/`。
2. Phase 11 公共用例接口和实现都进入 `server/phase11/business/<Component>/`；数据库和外部模型
   细节进入对应 adapter。新增类使用“一个组件一个目录”，让 `.h/.cpp` 保持相邻，同时不把
   PostgreSQL、curl、ONNX、HTTP 或 WebSocket 类型暴露给业务接口。
3. 浏览器代码只进入 `frontend/phase11/`；不要再放回通用 `static/`。
4. 单元测试、进程级黑盒、真实依赖 smoke 和 fuzz 分别进入现有四类目录。
5. 可执行脚本进入 `scripts/`，脚本生成的结果进入 `artifacts/`。
6. 当前设计放 `docs/architecture/`，数据库 Schema 学习材料放 `docs/database/`，一次性报告放
   `docs/reports/`，过期但有学习价值的材料放 `docs/archive/`。
7. 密钥、数据库数据、模型权重和临时 build 目录继续由 `.gitignore` 排除，不能放进任何文档或
   artifacts 目录。

## 5. Phase 11 组件索引

```text
server/phase11/
├─ ai/
│  ├─ ModelProvider/
│  ├─ ModelRegistryService/
│  ├─ OllamaModelProvider/        libcurl multi 调用与生成生命周期
│  ├─ OllamaStreamDecoder/        NDJSON 增量解码
│  ├─ InProcessOnnxModelProvider/
│  ├─ GrpcOnnxModelProvider/      聊天进程中的 gRPC 客户端
│  └─ GrpcOnnxWorker/             独立推理进程入口与服务
├─ business/
│  ├─ BusinessInterfaces/         HTTP、WS、SSE 共用的业务接口
│  ├─ ApplicationError/
│  ├─ AuthService/
│  ├─ SocialService/
│  ├─ ChatService/
│  ├─ AiChatService/
│  └─ OutboxDispatcher/
├─ domain/Domain/
├─ ports/Ports/
├─ runtime/BoundedExecutor/
├─ security/
│  ├─ InMemoryLoginRateLimiter/
│  └─ SodiumCredentialCodec/
├─ storage/
│  ├─ InMemoryStore/
│  └─ postgres/
│     ├─ PostgresConnectionPool/
│     ├─ PostgresStore/
│     └─ migrations/
├─ training/TrainingDataService/
└─ transport/
   ├─ Contracts/
   ├─ FlatJson/
   ├─ http/
   │  ├─ Phase11AuthHttp/
   │  └─ Phase11SocialChatHttp/
   └─ realtime/Phase11Realtime/
```

这套结构与 `server/http/HttpParser/`、`server/websocket/WebSocketSession/` 的规则一致：上层目录表达
技术或业务类别，最末级目录表达一个可独立阅读、编译和测试的组件。
