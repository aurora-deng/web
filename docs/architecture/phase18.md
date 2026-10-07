# Phase 18：AI 生成韧性与资源治理

## 1. 本阶段解决什么问题

Phase 17 结束时，聊天、文件和 AI 生成链路都能运行，但 AI Provider 仍有一个生产前风险：接口是异步
回调模型，如果底层库接受任务后既不返回 Token，也不触发完成回调，应用层无法证明请求已经结束。
结果可能是活动表一直保留、用户无法再次生成、关闭服务器一直等待，监控却看不出原因。

可以把每次生成想成餐厅的一张点菜单：Provider 接单后必须在规定时间内“出菜”或“报失败”。Phase 18
加入一个统一值班经理，它不替厨师做菜，只管理点菜单的期限、并发名额、故障次数和备用厨房。

本阶段完成：

1. `AiChatService` 的全局、每用户、每 AI 账号三级在途配额；
2. 一个集中式业务期限看门线程，不为每个请求创建线程；
3. 用户取消、响应过大、超时和进程停机时的立即摘除与唯一完成事件；
4. `ResilientModelProvider` 的 Provider 级集中看门线程；
5. 主/备用端点、Closed/Open/HalfOpen 熔断状态机；
6. 只有主端点尚未输出 Token 时才允许备用切换；
7. `/metrics` 中的 AI 业务和 Provider 韧性指标；
8. Linux Release、Debug、Sanitizer 和故障注入测试。

本阶段没有下载新依赖，没有更换模型，没有把本地代码推送到远程仓库。

## 2. 两层期限为什么都需要

```text
ai.generate
   ↓
AiChatService 业务总期限（默认 330 s）
   ↓
ResilientModelProvider 单端点期限（默认 300 s）
   ↓
Ollama / 进程内 ONNX / gRPC ONNX 自身期限
```

- **具体 Provider 的期限**最了解自己的网络或推理行为。例如 Ollama 的默认请求期限是 120 秒，gRPC
  有 RPC deadline。正常故障应先由它报告。
- **韧性包装器的期限**防止 Provider 实现本身失联。它会让旧回调失效、记录故障、打开熔断器，并在
  安全时尝试备用端点。
- **业务总期限**是最后保险。它保证无论下层发生什么，浏览器最终都能收到 `ai.completed`，并释放
  用户与 AI 账号的配额。

业务期限默认比包装器长 30 秒，目的是先让下层完成准确归因和备用切换。如果把业务期限设得更短，
功能仍会收尾，但熔断器可能只看到外部取消，无法把它记作端点故障。

两个看门器都是“一个线程看全部时钟”，而不是“一张点菜单站一个人”。`AiChatService` 等到最近的
deadline；Provider 包装器每 10 ms 检查活动表和外部取消。因此 1000 个在途生成不会额外产生 1000
条看门线程。

## 3. 并发配额

请求进入数据库执行器前先在同一把状态锁下检查：

| 配额 | 默认值 | 保护目标 |
|---|---:|---|
| 全进程活动生成 | 64 | 防止模型、数据库和 SSE 被无限请求压满 |
| 单用户活动生成 | 2 | 防止一个账号占完全部容量 |
| 单 AI 账号活动生成 | 16 | 防止一个热门模型节点形成无界排队 |

任一上限命中即抛出 `ErrorCode::RateLimited`。WebSocket 适配层把它转换为明确失败响应，而不是先把
请求放进无限队列。配额只统计尚未终结的 AI 生成；普通聊天、已持久化消息和历史查询不受影响。

请求在以下任一状态都会释放名额：成功、Provider 失败、用户取消、响应超过 16 KiB、业务期限到期、
数据库持久化失败和服务器关闭。完成路径使用“从活动表原子摘除”保证重复或迟到回调不会第二次通知。

## 4. 熔断器状态机

```text
                 连续失败达到阈值
Closed  ─────────────────────────→  Open
  ↑                                    │
  │ 探针成功                           │ 开窗时间结束
  └────────────── HalfOpen  ←──────────┘
                      │
                      └─ 探针失败 → Open
```

- **Closed**：正常放行，请求成功会清零连续失败数；
- **Open**：默认 30 秒内不再访问故障端点，直接尝试备用端点或返回不可用；
- **HalfOpen**：开窗结束后只放行一个探针，避免恢复瞬间所有流量一起冲回去；
- 默认连续 3 次非取消失败后打开；用户主动取消不算端点故障。

熔断器像配电箱里的空气开关。它不能修好模型服务，但能阻止已知故障不断消耗连接、线程和超时时间。

## 5. 备用端点为什么只允许同 Runtime

`ModelRouter` 已根据激活版本填入 `modelArtifact` 和 `adapterArtifact`。Ollama 的工件通常是模型标签，
ONNX 的工件则是模型目录；直接从 Ollama 切到 ONNX 会把“菜单编号”交给完全不同的厨房，格式和模型
版本都可能不兼容。

因此 `ResilientModelProvider` 构造时强制主、备 `name()` 相同。目前启动入口支持：

```text
WEB_PHASE11_OLLAMA_URL             主 Ollama 地址
WEB_PHASE11_OLLAMA_FALLBACK_URL    可选备用 Ollama 地址
```

两端收到相同模型标签。ONNX Provider 当前也经过熔断和超时包装，但启动脚本尚未配置第二个 ONNX
端点；以后可用同样规则加入第二个 gRPC Worker。

还有一条比“同 Runtime”更重要的限制：**主端点一旦输出过任何 Token，就不再切备用端点**。假设主
模型输出“数据库事务应该”，随后失败；备用模型从头输出另一种答案，如果直接拼接，浏览器会看到语法
通顺却逻辑错误的混合文本。当前实现宁愿明确结束为失败，也不伪造完整回答。

## 6. 取消、超时和迟到回调

每个韧性请求包含两种取消信号：

- 外部 Token：属于用户/业务生命周期；
- 当前端点 Token：属于这一轮主端点或备用端点。

主端点超时时，只取消当前端点 Token，外部请求仍可交给备用端点。用户取消时，两者都结束。每启动
一轮端点调用都会增加 `serial`；旧端点之后才到达的 Token/完成回调携带旧 serial，会被直接忽略。
这相当于给每次接力棒编号，上一棒迟到后不能再跑进当前赛道。

`AiChatService::cancel()` 先点亮取消信号，再立即从业务活动表摘除并发布 cancelled 终态，不等待底层
库是否配合。Provider 包装器最多 10 ms 后观察外部取消，释放自己的活动记录。服务器 `shutdown()`
也会先摘除全部活动请求、发布终态并唤醒看门线程，因此故障 Provider 不能无限拖住退出。

## 7. 启动配置

| 环境变量 | 默认值 | 允许范围 | 含义 |
|---|---:|---:|---|
| `WEB_PHASE11_AI_MAX_ACTIVE` | 64 | 1–65536 | 全进程活动生成上限 |
| `WEB_PHASE11_AI_MAX_PER_USER` | 2 | 1–1024 | 单用户活动生成上限 |
| `WEB_PHASE11_AI_MAX_PER_ACCOUNT` | 16 | 1–65536 | 单 AI 账号活动生成上限 |
| `WEB_PHASE11_AI_PROVIDER_TIMEOUT_MS` | 300000 | 100–3600000 | 单个主/备端点期限 |
| `WEB_PHASE11_AI_DEADLINE_MS` | 330000 | 100–3600000 | 业务总期限 |
| `WEB_PHASE11_AI_CIRCUIT_FAILURES` | 3 | 1–100 | 打开熔断器所需连续失败数 |
| `WEB_PHASE11_AI_CIRCUIT_OPEN_MS` | 30000 | 100–3600000 | Open 状态保持时间 |
| `WEB_PHASE11_OLLAMA_FALLBACK_URL` | 未设置 | URL | 可选同模型 Ollama 备用端点 |

实际部署应保持 `AI_DEADLINE_MS > AI_PROVIDER_TIMEOUT_MS`，并让具体 Provider 的网络/RPC 期限小于或
等于包装器期限。用户态服务脚本已经显式导出默认值，便于排查运行进程使用的策略。

## 8. 监控指标

`/metrics` 新增：

```text
webserver_phase11_ai_accepted_total
webserver_phase11_ai_quota_rejected_total
webserver_phase11_ai_succeeded_total
webserver_phase11_ai_failed_total
webserver_phase11_ai_cancelled_total
webserver_phase11_ai_timed_out_total
webserver_phase11_ai_active
webserver_phase11_ai_provider_requests_total
webserver_phase11_ai_provider_primary_failures_total
webserver_phase11_ai_provider_fallback_attempts_total
webserver_phase11_ai_provider_fallback_successes_total
webserver_phase11_ai_provider_fallback_failures_total
webserver_phase11_ai_provider_circuit_opened_total
webserver_phase11_ai_provider_circuit_rejected_total
webserver_phase11_ai_provider_timed_out_total
webserver_phase11_ai_provider_active
```

排障时先看 `ai_active` 是否持续不降，再看 `timed_out`、`quota_rejected` 和 `circuit_opened`。备用成功
只能说明请求被另一端点完成，不能掩盖主端点故障；因此主失败与备用成功分别累计。

## 9. 代码索引

| 文件 | 职责 |
|---|---|
| `server/phase11/business/AiChatService/AiChatService.h` | 配额/期限配置与业务指标 |
| `server/phase11/business/AiChatService/AiChatService.cpp` | 活动表、统一看门线程、取消和停机回收 |
| `server/phase11/ai/ResilientModelProvider/ResilientModelProvider.h` | 韧性配置、指标和 Provider 装饰器接口 |
| `server/phase11/ai/ResilientModelProvider/ResilientModelProvider.cpp` | 熔断、主备切换、端点期限、serial 防迟到回调 |
| `main.cpp` | 环境变量、Provider 包装、Ollama 备用端点和指标装配 |
| `scripts/phase11/webserver-user-service.sh` | Linux 用户态服务默认治理参数 |
| `tests/phase11/unit/ai_chat.cpp` | 配额、业务超时、取消和停机故障注入 |
| `tests/phase11/unit/ai_resilience.cpp` | 熔断、半开恢复、备用、超时与外部取消测试 |

## 10. 验证

新增测试使用可控的假 Provider 注入以下故障：

1. 返回失败且未输出 Token，确认备用成功；
2. 熔断窗口内确认主端点不会被再次调用；
3. 开窗结束确认单探针成功并关闭熔断；
4. 主端点先输出 partial 再失败，确认禁止备用拼接；
5. Provider 永不回调，确认端点超时后备用接管；
6. 外部取消而 Provider 不回调，确认活动记录仍会清零；
7. 业务 Provider 永不回调，确认统一期限终结且配额可回收；
8. 停机时 Provider 永不回调，确认 `shutdown()` 有界返回。

2026-10-07 在 CentOS Stream 9 虚拟机完成：

| 验证 | 结果 |
|---|---|
| Release 全量 CTest | 95/95，30.28 秒 |
| Debug 全量 CTest | 95/95，30.74 秒 |
| ASan + LSan + UBSan 全量 CTest | 95/95，36.12 秒 |
| AI 韧性专项重复 | 包含 5 ms 完成/超时竞态，连续 20 轮通过 |
| TLS/ALPN 黑盒稳定性 | 修正半包读取假设后连续 10 轮通过 |

Debug 第一次全量曾在 TLS HTTP/1 检查中出现一次瞬时失败。原因是测试只调用一次 `recv()` 就断言完整
状态行，而 TCP/TLS 不保证一次读取返回完整头部；服务端协议实现本来就允许半包。测试改为读取到
`\r\n\r\n` 或上限后再判断，单项连续 10 轮及随后完整 Debug 全量均通过。三套全量没有 Sanitizer
报告，也没有 AI 活动记录、看门线程或 Provider 回调泄漏。

## 11. 适用场景、优缺点与下一步

这套机制适合单进程聊天服务访问本地或远程模型端点，尤其适合 Ollama 双端点、独立 ONNX Worker 和
开发环境中经常启停的模型服务。优点是没有新增第三方依赖、资源有界、故障行为明确；代价是熔断状态
目前只存在当前进程内，多实例部署时每个实例会独立判断健康，配额也不是全局共享。

下一步应按顺序补：

1. 生成运行记录与模型/Adapter 启用审计；
2. 请求级 trace id、首 Token 延迟和总耗时直方图；
3. 固定评测集与模型版本发布门禁；
4. RAG 的文档切分、检索权限和引用追踪；
5. 在已经验证的 Adapter-ready ONNX 图上进行真实 LoRA 训练与质量对照。

完成审计与评测之前，不把“能生成”描述成“模型质量已达到生产标准”。
