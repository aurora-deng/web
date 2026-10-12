# Function2.1 与 Function2 对比，以及下一步演进建议

日期：2026-10-12。目标分支 `Function2.1`；基线为远程 `Function2` 的 `f4743a59240d774e86dcd642be779125894c721d`，已在本次核对一致。

## 1. 本版本定位

Function2 已具备聊天、文件、AI Provider、模型治理和运维工具。Function2.1 把聊天界面的“异常也能恢复”补齐，并让源码讲义按通信与业务职责组织。

| 项目 | Function2 基线 | Function2.1 |
|---|---|---|
| 会话初次打开 | 最早一页、最多 100 条 | 默认最新 100 条；排他 `before` 游标向前翻页 |
| 断线补发 | 单页恢复难以覆盖大缺口 | WS/SSE 重连后获取水位，逐页追赶；重查可见窗口的撤回状态 |
| 发送确认 | accepted/广播确认成功，超时缺少主动核对 | 5 秒超时先查自己的原幂等键，查无记录才按原 ID/内容有限重试 |
| 本机断线或刷新 | 未确认命令缺少恢复队列 | 当前账号的待确认文字命令保留在标签页 sessionStorage，最多 32 条 |
| 管理撤回 | 后端有规则，前端只有本人入口 | 角色允许时显示“管理撤回”；后端仍校验成员、角色和两分钟窗口 |
| 教材组织 | phase 文档集中在 architecture | `communication/` 收录通信层，`business/` 收录业务层 |
| 架构学习 | 各阶段说明 | 端到端总图、通信/业务调用图、业务接入服务器的分课讲义 |
| 测试 | 已有核心/协议/业务测试 | 新增可靠性 C++、前端状态机、Chrome 故障交互和真实接口回归 |

没有新增协议或第三方依赖，没有新增数据库迁移。仍保留原有 `messages + outbox_events` 事务、发送幂等约束、数据库执行器、网络线程与单写者发送体系。

## 2. 主要文件与阅读入口

1. [Phase 27](business/phase27.md)：四项实现的流程、边界、接口、详细代码索引和测试。
2. [ChatHistory.js](../../frontend/phase11/chat/ChatHistory.js)：最新/旧页、同步目标、连续游标、撤回终态合并和切会话版本隔离。
3. [ReliableSender.js](../../frontend/phase11/chat/ReliableSender.js)：ACK 超时核对、固定 ID 重试、有界队列、刷新恢复。
4. [app.js](../../frontend/phase11/app.js)：浏览器按钮、重连编排、状态渲染、管理撤回。
5. [SocialChatHttp](../../server/phase11/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.cpp)：`before` 查询和 `messages/by-client-id` 接口。
6. [ChatService](../../server/phase11/business/ChatService/ChatService.cpp)、[Ports](../../server/phase11/ports/Ports/Ports.h)、[PostgresStore](../../server/phase11/storage/postgres/PostgresStore/PostgresStore.cpp)：业务权限、仓储抽象与现有索引的查询实现。
7. [端到端架构](ARCHITECTURE.md)、[通信调用图](communication/protocol-call-flows.md)、[业务调用图](business/business-call-flows.md)、[业务接入讲义](business/server-business-integration-tutorial.md)。

目录调整是迁移教材与修正链接，原文仍保留；移动造成的 Git 删除/新增记录不代表教材内容被清理。

## 3. 验证与证据边界

以下为上一轮 Phase 27 在本次源码实现上的验收记录；本次发布只更新版本/导航/建议文档，没有重新声称跑过 Linux 全量测试。

| 验证 | 已记录结果 |
|---|---|
| Linux Release 全功能构建 | PostgreSQL、libsodium、curl、ONNX、gRPC 开启，构建通过 |
| Linux 完整 CTest | 100/100，0 失败 |
| Node 前端单元测试 | 13/13 |
| Chrome 实际页面 + 可控协议替身 | 5 组通过，无 JavaScript 页面错误 |
| 真实 HTTP/WS + PostgreSQL | 3 组通过：分页/授权、幂等/冲突、管理撤回与 129 条离线补齐 |
| ASan/LSan/UBSan | 本次核心 C++ 可靠性测试通过；不是完整服务器压力测试 |

隔离 Linux 目录：`/home/pikachu/phase27-chat-reliability`；测试库：`phase27_chat_test`；测试端口：18091。测试服务在验收后已停止，原运行目录没有替换。

这次 Git 同步只发布源码、测试和教材，不发布数据库、真实账号会话、密码、密钥、模型权重或本机构建压缩包。GitHub 分支更新不等于虚拟机服务自动部署。

## 4. 推荐下一步：Phase 28 真实业务容量与性能闭环

把目前的服务器理解成一座已经能营业的服务站：这一版补好了漏单和补单流程，下一步应确认“同时来了很多人，会先堵在哪个窗口”。推荐以下顺序，而不是把所有模块一起重写。

| 顺序 | 要做的工作 | 为什么现在做 | 完成证据 |
|---|---|---|---|
| 1 | 复用 Phase 15/26，做三轮同配置聊天混合负载基线 | `/fast` 上限与真实消息事务成本不同；Phase 26 当前缺少正式容量归档 | 吞吐、P50/P95/P99、错误率、环境和原始结果，报告三轮中位数 |
| 2 | 在基线加入本版分页/恢复/重试场景 | 断线恢复会增加历史查询，丢 ACK 会增加核对请求 | 多客户端重连时不重复保存，不漏消息，数据库/出站队列保持有界 |
| 3 | 按测量拆分 Worker 等待、连接池等待、SQL、Outbox 和发送耗时 | 每层已有队列，平均 QPS 不能定位尾延迟 | 请求/消息 ID 关联，阶段耗时及队列水位能解释 P99 |
| 4 | 对最明显瓶颈做单项优化，再 A/B | 优化需要证明收益，同时保住可靠性 | 同硬件、同数据量、同配置，零业务错误，回归通过 |
| 5 | 固化启动/迁移/备份恢复/发布回滚和长稳验收 | 把学习项目变成可复现运行的版本 | 干净构建、隔离数据库、故障演练与恢复报告 |

第一轮场景应包括：私聊持续写入、群聊广播、历史最新/旧页、离线跨页追赶、ACK 核对请求、长驻 WS/SSE、普通聊天与 AI 同时工作。AI 准备和模型首次加载应与稳态计时区间明确分开。

最低门槛是业务错误与重复写入为 0、队列/连接资源有界、取消或断线后能收尾；P99 和吞吐目标应依据实际虚拟机配置设定，不预填一个未经测量的生产承诺。

### 4.1 优先关注的架构位置

1. **通用 Worker 等数据库结果。** 当前 `SocialChatHttp::runDatabase()` 将 SQL 放进 DatabaseExecutor 后调用 `future.get()`。Reactor 没有执行 SQL，但发起业务的 Worker 仍被占用。若排队指标证明这里是瓶颈，再引入 continuation/协程挂起；先完成连接身份、响应所有权、取消、超时和关闭收尾设计。
2. **查询次数与访问计划。** 看分页和消息附件补全是否产生过多往返，再考虑批量读取和索引。可以先用 PostgreSQL 自带 `EXPLAIN (ANALYZE, BUFFERS)` 在隔离数据上诊断；`ANALYZE` 会真实执行语句，不拿正式库做写操作诊断。[PostgreSQL EXPLAIN 官方文档](https://www.postgresql.org/docs/current/sql-explain.html)
3. **重连恢复的资源预算。** 当前每个标签页合并同步请求，但很多客户端一起重连仍会形成查询高峰。依据测量考虑客户端抖动、分页请求节流和服务端每用户恢复预算；保留原幂等 ID 与完整追赶语义。
4. **AI 与普通聊天隔离。** 观察 AI Worker 排队、首 token 和聊天 P99，优化流式缓冲、配额及取消收尾。gRPC 流写入完成不代表字节已经到对端，不能用它代替业务 ACK。[gRPC 流控官方说明](https://grpc.io/docs/guides/flow-control/)

### 4.2 更远一点的方向

1. 单机性能与恢复稳定后，先把已有独立 gRPC 推理 Worker 的运行、监控和故障隔离做好，这是当前最自然的服务边界。
2. 需要聊天多实例部署时，再设计跨节点通知、共享 Presence/会话、Outbox 并发消费与租约、跨节点限流；新增消息中间件或缓存前仍须依赖审批。
3. 模型方向用授权数据与独立评测集比较基线/候选，明确回滚门槛。真实质量报告完成后再推进更复杂训练或 RAG 能力。

以上仅为演进建议，本次没有实施 Phase 28，也没有安装新依赖或启动容量压测。
