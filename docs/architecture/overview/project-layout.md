# 项目目录与构建边界

文档主题与阶段时间线见 [文档导航](../../README.md)；移动记录见 [document-moves.json](../../document-moves.json)。本次文档位置调整不改变这里描述的源码和部署边界。

当前源码按运行位置和职责分类；Phase36–41仍在本地验收，日常实例没有切换。历史移动记录保存在 [移动清单](../source-layout-migration.json)，不要重新执行一次性迁移脚本。

```text
项目根目录
├─ backend/
│  ├─ server/                  Linux通信内核：Reactor、Connection、HTTP、WS、SSE、H2、TLS、gRPC
│  ├─ application/             共享业务：账号、社交、聊天、文件、动态、神树、榜单、管理与Outbox
│  │  ├─ bootstrap/            进程与领域资源装配、生命周期
│  │  ├─ business/             既有业务服务及兼容接口
│  │  ├─ modules/              Tree / Activities / Assistant / Admin / Messaging 等隔离模块
│  │  ├─ ports/ domain/        既有领域与Repository端口
│  │  ├─ storage/              内存实现、PostgreSQL实现与追加式迁移
│  │  ├─ security/             密码、Session、CSRF、限流
│  │  ├─ runtime/              有界业务/数据库执行器
│  │  └─ transport/            HTTP/WS/SSE业务DTO、适配与路由注册
│  └─ ai/
│     ├─ models/               Provider、模型注册、RAG、评测、gRPC推理Worker
│     ├─ private/              无正文落库的限时私人推理编排与端口
│     ├─ speech/               语音策略、协议、云适配、独立Kokoro Worker
│     └─ training/             授权数据、审核、数据集导出与训练控制
├─ client/
│  ├─ companion/              Windows本机入口、HTTP/WS/SSE代理、Schannel、Host/Origin保护
│  ├─ assistant/              私人线程、授权词法检索、记忆、确认操作、迁移、个人语音
│  └─ storage/                Windows SQLite、NTFS目录权限、DPAPI凭据
├─ frontend/phase11/
│  ├─ shared/                 导航、主题、会话、统一发布器、私人助手、语音设置
│  ├─ chat/                   聊天列表、可靠发送、历史恢复
│  ├─ accounts/ contacts/     账号、联系人及领域API绑定
│  ├─ ai/ knowledge/ files/   AI、资料、文件控制器与绑定
│  ├─ tree/                   3D场景、广场、榜单、果实详情
│  ├─ profile/                个人资料与统一动态
│  ├─ assistant/              好友式私人助手独立页
│  └─ admin/                  站点管理、模型、公告音色与运行状态
├─ tests/                     按通信、业务、Function3、本机客户端分类
├─ scripts/                   按运行、依赖准备、验证、训练与目录迁移分类
├─ benchmarks/                可复现网络和真实业务基准
├─ docs/
│  ├─ architecture/communication/  按Reactor/HTTP/WS/SSE/H2/TLS/gRPC分类
│  ├─ architecture/business/       按账号聊天/AI/社区/客户端/运行/工程分类
│  ├─ database/                    表含义、权限、事务与迁移说明
│  ├─ testing/ performance/ ops/   验证、性能证据与启动说明
│  └─ reports/ archive/            历史结果与参考资料
├─ tools/                     显式运行的管理、评测、数据与模型工具
├─ artifacts/                 经筛选可分享的历史验证证据
├─ CMakeLists.txt             Linux装配与可选依赖开关
└─ main.cpp                   进程入口；ApplicationRuntime及领域Runtime装配/注册/停止
```

## 依赖方向

1. HTTP/WebSocket适配器调用Application Service；业务服务只依赖领域对象和Port。
2. PostgreSQL、Redis、RocketMQ、云语音等驱动实现Port，不能把驱动类型送进业务接口。
3. `webserver_application_support`提供便携执行器与DTO支持；`webserver_ai`不依赖Reactor；协议到私人推理的HTTP适配由独立目标装配。
4. Windows有独立的 `client/CMakeLists.txt`。共享语音策略不依赖Windows/Linux平台类型；WinHTTP和curl分别留在平台驱动内。
5. 目录分离不等于进程隔离，也不自动提高性能。实际独立进程是ONNX gRPC Worker、Kokoro Worker和显式执行的转换/训练任务。

## 文件创建与数据位置

C++组件使用同名目录，`.h`和`.cpp`放在一起；纯接口不制造空实现。小型紧密相关辅助文件留在所属组件目录。前端公共组件同样独立分组，不把新功能堆进页面入口。

`build-*`、下载缓存、数据库、用户文件、模型、证书私钥、云Key、私人历史与训练数据不提交Git。构建目录是可再生结果，测试语料属于tests，验收结果属于artifacts或明确的忽略目录。

Linux原业务库保持权威；Windows仅新增账号隔离的私人SQLite，不创建另一份公共聊天库。数据库升级只追加迁移文件。启动、备份、切换日常服务需要按运维文档显式执行，本轮不自动替换。

相关文档：[端到端总图](../business/flows/phase36-41-end-to-end.md)、[Phase38本机入口](../business/client-ui/phase38.md)、[Phase39私人AI与确认操作](../business/ai/phase39.md)、[Phase40语音](../business/ai/phase40.md)。
