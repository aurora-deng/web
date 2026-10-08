# Phase 26：长连接容量、AI 并发与混合负载证据

Phase 26 补齐“功能已经能用”和“负载上来仍然能用”之间的证据缺口。它不新增聊天业务，而是把
WebSocket 长驻连接、真实 AI 生成以及普通聊天和 AI 同时工作时的隔离效果纳入同一套可重复基准。

可以把 Phase 15 想成让汽车跑完城市道路，Phase 26 则继续让它满载爬坡：不仅看能不能到终点，还要
记录载客增加后的油耗、温度和速度变化。

## 1. 当前状态

截至 2026-10-08：

- 基准代码、聚合器、隔离数据库驱动和保护脚本已经完成；
- Python 编排测试本地 **4/4 通过**，语法编译和 `git diff --check` 通过；
- 正式 24 小时长稳仍在采样，因此尚未运行会暂停 18090 服务的 Phase 26 独占测试；
- 本文没有提前填写 QPS、P99、TTFT 或每连接内存，也没有把“工具已完成”写成“容量已证明”。

正式结果应在长稳汇总生成后运行三轮，再把 `summary.json` 和环境记录复制到
`docs/evidence/phase26/`。在此之前，本阶段状态是**测试能力已就绪，真实容量证据待采集**。

## 2. 补的是哪些缺口

| 原有证据 | 缺口 | Phase 26 补充 |
|---|---|---|
| WebSocket 反复连接/关闭 | 没有连接一直在线时的容量和内存斜率 | 50 → 200 → 500 条长驻连接阶梯 |
| SSE ready 重连 | 已能验证断线重建 | 保留原场景，与 WS/AI 一起进入同一报告 |
| AI 生成函数已经存在 | 驱动没有给隔离库创建真实 AI 账号和模型绑定 | 用已验证模型制品创建专用 AI 夹具 |
| 普通 HTTP/WS/SSE 混合负载 | 不知道模型推理是否拖慢普通聊天 | AI 生成和普通聊天同时起跑，分别记 P99 与 TTFT |
| 通用中位数聚合 | AI 专有字段会丢失 | 聚合 TTFT、token/s、连接内存和隔离退化比例 |

## 3. 完整测试流

```mermaid
flowchart TD
    Guard[确认 24h 长稳已结束] --> DB[重建隔离 PostgreSQL 数据库]
    DB --> Migrate[执行全部 Migration]
    Migrate --> Seed[绑定已验证 ONNX 模型和专用 AI 账号]
    Seed --> Server[18091 独占启动 Release 服务]
    Server --> Base[登录/列表/私聊/群聊/离线/SSE 基线]
    Base --> WS[WebSocket 50→200→500 长驻阶梯]
    WS --> AI[真实 AI 并发生成]
    AI --> Mixed[普通聊天 + AI 同时运行]
    Mixed --> Proc[/proc RSS、FD、线程、CPU]
    Proc --> Aggregate[三轮中位数与退化比例]
    Aggregate --> Restore[恢复 18090 正式服务]
```

准备账号、建会话和打开 AI 专用 WS/SSE 连接发生在计时区间外。否则把 Argon2 密码散列和会话准备
时间塞进 token/s，就像把运动员换衣服的时间算进百米成绩，数字会有格式但没有正确含义。

## 4. WebSocket 长驻容量

长驻测试默认依次保留 50、200、500 条连接。每到一个阶梯会：

1. 打开新增连接并保存 socket，不立即关闭；
2. 向全部存活连接写 WebSocket Ping，覆盖控制帧路径；
3. 等待分配器和事件循环稳定；
4. 读取服务进程 RSS、FD 和线程数；
5. 计算相对零连接基线的 `estimatedBytesPerConnection`。

同一账号最多允许四个 WebSocket 设备连接，因此工具按一名用户四个设备创建夹具。500 条连接对应
125 名用户，既覆盖多用户，也不会把 500 次昂贵的密码散列混进连接测量。

关键输出：

| 字段 | 含义 |
|---|---|
| `requestedConnections` | 本阶梯目标连接数 |
| `establishedConnections` | 实际握手成功并保留的连接数 |
| `deltaRssKiB` | 相对连接前基线增加的常驻内存 |
| `estimatedBytesPerConnection` | 增量 RSS ÷ 当前连接数，只用于容量估算 |
| `deltaFileDescriptors` | 服务进程新增 FD |
| `fileDescriptorsPerConnection` | 每条连接的 FD 斜率 |

RSS 会受内存分配器复用影响，所以低阶梯单点不能当作精确对象大小；应看三轮中位数和多个阶梯的整体
斜率。它适合做容量规划，不等于 C++ 类型的 `sizeof`。

## 5. AI 并发与隔离

AI 测试使用数据库中的真实 `ModelVersion`、真实 Provider 和真实 SSE token，不用固定文本假装模型响应。

```text
AI 用户夹具（计时外）
    ↓
所有 Worker 在起跑门等待
    ↓ 同时释放
ai.generate ──────────────┐
SSE ai.token / completed │
                          ├─ 同一个时间窗
HTTP 会话列表/历史 ──────│
WebSocket chat.send ─────┘
```

分别记录：

- AI：P50/P95/P99 总耗时、TTFT、token/s、平均 token 数、错误数；
- 普通聊天：混合业务吞吐、P50/P95/P99、错误数；
- 隔离对比：`ordinaryChatDuringAi.p99ChangePercent`；
- 反向影响：`aiDuringOrdinaryChat.ttftP95ChangePercent`。

这两个百分比像两间相邻教室的隔音测试：一间开始大声上课后，另一间的听课效果下降多少。它能判断
AI Worker、数据库执行器和 Reactor 的隔离是否真的生效。

## 6. 运行方法

先确认 Phase 23 的 `summary.json` 已生成且结论通过，再从模型清单取得**真实相对目录和摘要**：

```bash
cd /home/pikachu/phase11-full-src

export PHASE26_ALLOW_SERVICE_RESTART=1
export PHASE15_AI_MODEL_RUNTIME=onnx-inprocess
export PHASE15_AI_MODEL_ARTIFACT='<WEB_PHASE11_ONNX_MODEL_ROOT 下的相对目录>'
export PHASE15_AI_MODEL_CHECKSUM='<该模型目录的已验证 SHA-256>'

bash scripts/phase26-capacity-benchmark.sh
```

若模型使用 Adapter，再设置：

```bash
export PHASE15_AI_ADAPTER_ARTIFACT='<相对 .onnx_adapter 路径>'
```

脚本默认三轮、4 个普通业务 Worker、500 条最大长连接、AI 并发 4。可通过同名前缀环境变量调整，
例如 `PHASE15_WEBSOCKET_CONNECTION_STEPS=100,500,1000`。不得用一个随手写的摘要绕过 Provider 校验；
模型目录、摘要或 Adapter 不匹配时，本轮应失败并保留错误证据。

`PHASE26_ALLOW_SERVICE_RESTART=1` 是防误操作闸门，因为底层驱动会暂停 18090 正式服务；若用户态
Supervisor 正在运行，会先停止 Supervisor 和它托管的进程，避免测试期间被自动拉起。18091 独占测试
结束后，驱动按原状态恢复 Supervisor 或普通 Service。这个变量不是跳过测试或安全校验的开关。

## 7. 验收口径

首轮结论至少同时满足：

1. 三轮 `totalErrors == 0`；
2. 每个连接阶梯的 `establishedConnections == requestedConnections`；
3. 500 连接后没有持续增长的线程数，FD 斜率与 TCP 连接数量相符；
4. AI 生成有非零 token、完整 `ai.completed(success=true)`，TTFT 和总耗时均有三轮中位数；
5. AI 运行期间普通聊天仍然零错误；
6. 普通聊天 P99 相对同轮无 AI 基线的变化被明确记录，而不是只报告平均值；
7. 日志无崩溃、数据库错误、Outbox 丢失或 Backpressure 失控。

首次容量测量没有可信历史阈值，因此内存斜率和 AI 隔离退化先作为基线保存。第二次架构优化才使用同一
硬件、同一模型、同一提示词和同一编译配置设置回归门槛，避免先拍脑袋定数字再挑结果。

## 8. 文件索引

| 文件 | 作用 |
|---|---|
| `benchmarks/phase15_business_benchmark.py` | 长驻连接、AI 生成和 AI 混合负载实现 |
| `benchmarks/phase15_aggregate.py` | 三轮中位数、TTFT、连接斜率和隔离变化比例 |
| `scripts/phase15-business-benchmark.sh` | 隔离数据库、真实模型夹具、服务启停和证据目录 |
| `scripts/phase26-capacity-benchmark.sh` | Phase 26 固定容量矩阵与长稳保护闸门 |
| `tests/phase11/integration/business_benchmark_tools.py` | 参数、长连接编排、AI 起跑门和聚合契约测试 |
| `CMakeLists.txt` | `phase15_business_benchmark_tools` 测试注册 |

## 9. 与全部计划的关系

当前业务功能、消息生命周期、群聊权限、联系人、文件、AI Provider、生成治理、评测门禁、RAG、授权
数据集、LoRA 训练脚本、候选发布和进程监督均已有实现与对应阶段文档。仍未闭环的是：

- Phase 23 正在运行的 24 小时长稳最终汇总；
- 正式进程真实 `SIGKILL → 自动恢复 → readiness → 登录/消息恢复` 演练；
- 本阶段三轮真实容量矩阵；
- 使用真实授权训练数据得到候选模型后，完成质量对比并由人工决定是否发布。

因此项目已经进入“验证和定容量”阶段，而不是继续堆新页面或新协议。完成上面四项后，才能给出当前
硬件和当前模型条件下的生产容量结论；在此之前只能说功能闭环较完整，不能说全部生产验收完成。
