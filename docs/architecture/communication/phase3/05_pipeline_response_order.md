# 第三阶段重要知识点 · 05 HTTP Pipeline 与响应顺序

> **标记**：第三阶段重要知识点  
> **对应代码**：Parser 流水线用例、黑盒 pipeline；设计上的 Request/Response 队列

---

## 1. 什么是 Pipeline

客户端在同一 TCP 上连续发送：

```text
GET /a …\r\n\r\nGET /b …\r\n\r\nGET /c …
```

一次 `recv` 可能读到多份完整请求 → **HTTP/1.1 Pipelining**（与 Keep-Alive 相关但不是同一概念）。

---

## 2. 解析侧不变量（本仓已测）

| 不变量 | 证据 |
|--------|------|
| 半包：不够则 NEED_MORE | 单元 `WaitsForHalfPacket…` |
| 粘包/流水线：只消费当前请求，下一份留在 Buffer | 单元 `LeavesPipelinedRequestInBuffer` |
| 端到端一次写入多请求、按序应答不串包 | 黑盒 pipeline |

---

## 3. 为何需要「响应顺序」意识

若多个请求**并行**进 Worker，完成顺序可能是 2→3→1，但 HTTP/1.1 pipeline 要求响应顺序与请求一致：**1→2→3**。

因此完整体常有：

```text
RequestQueue  →  Executor（可乱序完成）→  按 RequestId 归位  →  ResponseQueue 按序 Sender
```

---

## 4. 本仓当前定位（读设计时勿混淆）

- **现状**：单连接上 Session 协程对请求 **串行** `读完整 → 提交 Executor → 等完成 → 写`；自然保持顺序。  
- **Buffer** 仍可囤后续请求字节（解析流水线）。  
- **演进点**：若将来同连接并行执行多个 handler，必须显式 Response 排序队列；那是增强，不是第四阶段协议必需前置代码。

---

## 5. 心智模型图

```text
TCP → readBuffer → Parser（一次一个完整请求）→ [可选 RequestQueue]
                         ↓
                    Executor
                         ↓
              [可选按序 ResponseQueue] → Sender
```
