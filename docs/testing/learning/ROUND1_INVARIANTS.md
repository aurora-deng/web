# 测试学习 · 第一轮：不变量 ↔ 用例

> 对应 [LEARNING_SCOPE.md](../LEARNING_SCOPE.md) 第一轮。  
> **本轮目标**：会用「不变量」说话，并能指到现有测试；**不写新测试代码**。  
> 学完自测：合上文档，口头答「自测题」一节。

---

## 0. 先记住一个思维切换

写功能时想的是：「怎么实现」。  
写/读测试时想的是：「**什么永远为真**（不变量），以及**一破就红**」。

例子：

- 功能说法：`/slow` 里 `sleep(10)`，业务扔到线程池。  
- 测试说法：**不变量**「Worker 上阻塞不得拖死同 Reactor 上其他连接」→ 黑盒 `/slow`+`/fast`。

四层测试**不互相替代**：

| 层 | 证明什么 | 不证明什么 |
|----|----------|------------|
| 单元 | 组件契约（Parser/Buffer/Router…） | 真 TCP、真线程调度 |
| 黑盒 | 进程外可观察行为 + Executor 边界 | 每个内部分支 |
| 压测 | 容量与 P50/P99 基线 | 正确性细节 |
| Fuzz | 解析器在垃圾输入下不崩 | 业务语义正确 |

`gen_report.sh` 绿 = 四层都有证据，不是「测完了永远没错」。

---

## 1. 单元测试：不变量 ↔ 用例（`tests/unit_tests.cpp`）

### Buffer

| 不变量 | 用例 |
|--------|------|
| 追加 / 压缩 / 扩容后，已写入字节仍连续可读；能找到 CRLF / 空行 | `BufferTest.AppendsCompactsAndFindsDelimiters` |

### HttpParser（口诀：半包要等、粘包要剩、冲突要拒、超限要死、交付要 reset）

| 不变量 | 用例 |
|--------|------|
| TCP 半包：不够则 NEED_MORE，凑齐后交付 | `WaitsForHalfPacketAndContentLengthBody` |
| 流水线：只消费当前请求，下一请求仍留在 Buffer | `LeavesPipelinedRequestInBuffer` |
| chunked 可跨多次喂入 | `DecodesChunkedBodyAcrossPackets` |
| Range 可解析；多段 / CL+chunked 冲突拒绝 | `ParsesByteRangesAndRejectsConflictingLengths` |
| 请求行 / 首部 / CL 有上限 | `EnforcesRequestLineHeaderAndContentLengthLimits` |
| chunked **累计**超限也拒绝 | `RejectsIncrementalChunkedBodyOverLimit` |
| 缺 Host、畸形、歧义定界等拒绝 | `RejectsAmbiguousRequestFramingAndMissingHost` |
| 交付后必须 reset；Connection token 可解析 | `RequiresResetAfterDeliveryAndParsesConnectionTokens` |

### Sender / Range / Router

| 不变量 | 用例 |
|--------|------|
| ResponseSender 可不依赖 SubReactor 发完 | `SendsWithoutSubReactor` |
| 闭 / 开 / 后缀 Range 经 Parser 可识别 | `HttpRangeTest.ParsesClosedOpenAndSuffixRangesViaParser` |
| `buildHeader` 不抹掉业务已设 Content-Range | `PreservesContentRangeForMemoryAndFileBodies` |
| `/user/:id` 匹配并抽参 | `MatchesDynamicRouteAndExtractsParameter` |
| 中间件可不调 `next`，短路 401 | `MiddlewareCanShortCircuitRoute` |
| 未匹配 → 看 **ctx.response**（池对象）为 404 | `ProducesNotFoundResponse` |

**注意**：断言点必须对准真正生效的对象（404 曾因断言栈上旧 `HttpResponse` 假红）。

---

## 2. 黑盒：不变量 ↔ 场景（`http_blackbox.py`）

| 不变量 | 怎么证 |
|--------|--------|
| 根路由与动态路由；同连接 keep-alive | `GET /` 再 `GET /user/123` |
| 鉴权中间件可短路 | `GET /admin` → 401 |
| chunked 可解码为完整业务体 | `/stream1` |
| 畸形版本 / CL+TE 不能当成功请求 | 连接关闭或非 200 |
| 超大声明正文触发限制 | 断开类行为 |
| 多请求 / pipeline 不串包 | keep-alive、pipeline |
| 静态 Range 区间长度 | `/logo` Range |
| **Executor 边界**：慢 handler 不堵快请求 | `/slow` 中 `/fast` 立刻成功 |

压测不在黑盒里：容量用 wrk。

---

## 3. 改 Runtime / Executor 时优先回归

1. 整份黑盒（尤其 `/slow`+`/fast`、pipeline）  
2. 单元：Router 404、Parser 半包/流水线、Sender  
3. 怀疑内存/竞争时再开 ASan **或** TSan（勿混开）

---

## 4. 自测题（合上文档再答）

1. 单元和黑盒本质差别？各举本仓一例。  
2. 「半包」和「流水线」各哪条用例？不变量？  
3. 为什么 404 要看 `context.response`？  
4. `/slow`+`/fast` 红了更可能坏哪层？Parser 全绿能排除什么？  
5. 「池满→503」今天有没有黑盒？算缺口还是必补？

### 参考批改要点（给批改人）

1. **不是**「单元=功能能不能用、黑盒=突发边界」这么切。更准：  
   - 单元 = **组件契约**（隔离测 Parser/Router…）  
   - 黑盒 = **整机可观察行为**（真进程 + TCP；含正常路径也含畸形/Executor）  
2. 半包 **和** 流水线 **都有单元**（`WaitsForHalfPacket…` / `LeavesPipelined…`）。黑盒另有 pipeline，但是「端到端不串包」，不是「只有黑盒测流水线」。  
3. 404 关键在 **Router 经 make404 换到的 `ctx.response`（池对象）**，不是「凡响应都看栈上那份」。  
4. 更可能坏 **Executor / Session 提交与唤醒**；Parser 全绿只说明 **解析契约** 大致仍在，排除不了线程边界。  
5. 当前 **无** 打满队列黑盒 → **已知缺口**，第一轮不必写码。

「四层」= 单元 / 黑盒 / 压测 / fuzz。  
「回归」= 改完代码再跑该红的用例；不是「触顶信号」。

---

## 5. 本轮结束标准

- [ ] 能说出四层各证明什么  
- [ ] 能从「不变量」指到用例  
- [ ] 知道动 Executor 边界优先回归什么  

合上本文，口头或对话回答「§4 自测题」；交卷口令：`第一轮交卷`（附答案）。  
参考批改：[ROUND1_ANSWERS.md](ROUND1_ANSWERS.md)（建议先答再看）。  
闭环现状：[CLOSED_LOOP_MAP.md](CLOSED_LOOP_MAP.md) 
