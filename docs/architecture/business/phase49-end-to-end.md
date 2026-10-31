# Phase49：三端、业务契约、事务与模型的整体调用图

本轮保持原三端部署和通信内核；新增的是业务领域归属、接口绑定和装配生命周期。先按图跟踪消息，再看私人助手及模型更新分支。

```mermaid
flowchart TB
    subgraph Browser[浏览器 frontend/phase11]
        UI[聊天 / 联系人 / 个人主页 / 神树 / 管理 / 助手]
        API[各领域 XxxApi + BusinessApi]
        RTUI[WS ACK、送达、已读 / SSE token、公告]
        UI --> API
        RTUI --> UI
    end
    subgraph Windows[Windows 可选本地客户端]
        Proxy[本机入口、固定目标代理、TLS 指纹校验]
        Private[私人线程、资料、词法检索、记忆、确认式工具]
        LocalDB[账号隔离 SQLite / DPAPI]
        Private --> LocalDB
        API -->|本地入口| Proxy
        UI -->|本地助手 API| Private
    end
    subgraph Linux[Linux 共享服务]
        subgraph Core[冻结的 backend/server]
            Socket[TCP / TLS、ALPN]
            Protocol[Reactor、协程、HTTP、H2、WS、SSE]
            Sender[身份核验、异步响应、发送队列与背压]
            Socket --> Protocol
            Protocol --> Sender
        end
        subgraph App[backend/application]
            HTTP[领域 HTTP / WS 适配器、Session、CSRF]
            Request[Query / Command 与可信 actor]
            Service[账号、联系人、聊天、文件、动态、神树、管理 Service]
            Port[Repository / Transaction Port]
            Facade[兼容 Store：当前 DB Worker 的事务连接]
            DAO[领域 DAO：参数化 SQL、行解码与投影]
            DB[(已有 PostgreSQL，迁移 024)]
            Outbox[提交后唤醒、Outbox、有界投递、可选 MQ / Inbox]
            Public[公开 VO / JSON / 实时事件]
            HTTP --> Request --> Service --> Port --> Facade --> DAO --> DB
            DB -->|业务数据 + 事件原子提交| Outbox
            DAO --> Public
            Outbox --> Public
        end
        subgraph AI[backend/ai]
            Model[模型路由、配额、取消、评测、Provider]
            Worker[独立 ONNX gRPC Worker / 教学进程内 / Ollama API]
            RAG[有权限的共享检索；私人片段只限时推理]
            Training[授权样本、审核、数据集版本、独立训练/转换任务]
            Gate[摘要与血缘、真实评测门禁、人工激活/回滚]
            Speech[语音配置、本地独立 TTS Worker、可选云适配]
            RAG --> Model --> Worker
            Training --> Gate --> Model
        end
        Assembly[ApplicationRuntime 与各领域 Runtime：资源所有权、显式停止]
        Assembly -.装配.-> HTTP
        Assembly -.装配.-> Model
        Assembly -.装配.-> Protocol
    end
    API -->|也可直接访问共享业务| Socket
    Proxy -->|HTTPS / WSS / SSE| Socket
    Private -->|用户授权的必要片段；独立私人推理契约| Socket
    Protocol --> HTTP
    Service --> RAG
    Service --> Model
    Service -->|用户明确授权后进入候选| Training
    Outbox --> Speech
    Speech --> Public
    Public --> Sender --> RTUI
    Public -->|HTTP 返回| Sender
```

## 必须分清的边界

1. 前端绑定统一了方法和路径，页面状态和业务权限仍分别归前端及服务端。服务器不信任按钮是否显示。
2. DB Worker 中的一次事务覆盖消息与 Outbox；网络 Reactor 不运行阻塞 SQL，领域 DAO 不另借连接拆散事务。
3. 业务公开投影与 DAO 行分别定义；用户搜索不暴露凭据，撤回/隐藏内容在每个读取入口重新过滤。
4. Windows 私人资料原文和索引留本机，授权片段可能发送自有 Linux 推理；这不是所有处理都在本机。网页助手需要非保密上传同意。
5. 普通聊天、群 AI 和私人助手使用不同的历史/知识边界；群 AI 不能读取个人私人知识。TTS 与推理 Worker 故障不得阻塞普通消息保存。
6. 训练、转换与模型发布是既有显式任务和评测门禁，不由普通聊天或动态发布自动触发；本轮没有训练新模型，也没有证明模型质量提高。

实现索引见 [Phase49 组件](phase49-code-index.md)，数据库对象与事务见 [领域分层规范](../../database/domain-layering.md)。底层协议细节见 [通信架构](../communication/README.md)。
