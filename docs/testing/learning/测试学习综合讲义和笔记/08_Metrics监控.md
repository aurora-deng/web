# 08 · Metrics 监控（详细讲义）

> 压测时 QPS 掉了——**外面**的 wrk 只告诉你「慢了/少了」。  
> **Metrics** 回答：**服务器肚子里发生了什么？**  
> 连接是不是在泄漏？线程池队列是不是堆成山？错误是不是在涨？

---

## 我对 Metrics 的理解（笔记）

**容易想歪的点（已纠正）：**

> 一开始容易觉得 Metrics = 另开一条线程一直「偷窥」服务器。  
> **更准确的理解：在业务路径上埋点统计 + 提供统一读取出口（如 `/metrics`）。**

**核心定义（自己的话）：**

> **Metrics = 程序运行状态的数字化描述**（请求数、错误数、活跃连接、队列长度、延迟分位……），让我在压测/故障时能看见肚子里发生了什么。

怎么记：

```text
accept 时连接 +1
close 时连接 -1
请求完成 请求数 +1
        │
     Metrics
        │
   GET /metrics 读出来
```

和 Benchmark 联读：外面 QPS 掉了，里面看是队列堆了、CPU 满了，还是连接只增不减（泄漏）。

---

## 1. 先纠正一个常见误解

### 错的想象

```text
另开一个「监控线程」
while (true) {
  偷偷扫描整个服务器状态;
}
```

问题：

1. **不知道业务时刻**：请求刚完成还是刚 accept，扫描线程很难精确知道。  
2. **太贵**：万级连接时反复遍历 map，本身拖垮性能。

### 对的模型：埋点 + 读出口

```text
accept() 里：连接数 +1
close()  里：连接数 -1
请求完成：请求数 +1，记下耗时
        │
        ▼
     Metrics 对象（原子计数 / 每线程本地再汇总）
        │
   GET /metrics   ← 统一读取出口
        │
  （可选）Prometheus / Grafana
```

所以：

> Metrics 工程化 **不是**「旁路偷窥线程」，而是 **业务路径记账 + 查询接口**。

---

## 2. Metrics 到底是什么？

**程序运行状态的数字化描述。**

例如：

| 数字 | 含义 |
|------|------|
| `total_requests` | 处理过多少请求 |
| `error_total` | 失败多少 |
| `active_connections` | 当前还有多少条 TCP |
| `queue_size` | 线程池还积压多少任务 |
| `latency_p99` | 尾巴延迟 |

有了这些，才谈得上「可观测」。

---

## 3. 在你的架构里哪里埋点？

对照第三阶段：

```text
Acceptor/accept     → active_connections++
Connection 析构/关闭 → active_connections--
完整请求进入处理    → request_total++
成功发送            → success++
400/404/500         → error++
```

延迟：

```text
请求解析完成：记下 start
响应发送完成：end - start → 记入延迟样本
```

粗算 QPS：

```text
QPS ≈ (当前总请求数 - 一段时间前总请求数) / 秒数
```

---

## 4. 为什么要用 atomic，而不是一把大锁？

错误：

```cpp
mutex 锁住;
request++;
解锁;
```

QPS 到很高时，**锁本身变成瓶颈**——你统计性能的动作反过来毁了性能。

更好：

```cpp
struct Metrics {
    std::atomic<uint64_t> request_total{0};
    std::atomic<uint64_t> error_total{0};
    std::atomic<uint64_t> active_connections{0};
};

metrics.request_total.fetch_add(1, std::memory_order_relaxed);
```

### 延迟样本怎么办？

延迟是一串数，不好塞进单个 atomic。工业常见：

```text
每个 SubReactor / Worker 线程各自有 ThreadLocal 统计
最后再汇总到全局
```

这正好符合「一线程一 Reactor、少共享」的设计。

---

## 5. 对外暴露：`GET /metrics`

最简单：在 Router 挂一个处理器，返回文本：

```text
server_requests_total 100000
server_connections 500
server_errors_total 20
server_latency_p99_ms 8
```

浏览器或脚本访问：

```text
http://127.0.0.1:8080/metrics
```

这就是 Prometheus 风格的雏形：**拉模型（pull）**——外部定期来问你。

教学伪代码：

```cpp
router.get("/metrics", [](Context& ctx) {
    ctx.response.setBody(Metrics::instance().dump());
});
```

---

## 6. 和 Benchmark 怎么联读？（最有用）

wrk 说：QPS 下降。打开 `/metrics`：

| 你看到 | 更可能的结论 |
|--------|----------------|
| `queue_size` 飙到几万，CPU 一般 | Worker/Executor 处理不过来 |
| `queue_size≈0`，CPU 100% | 纯计算/拷贝/解析瓶颈 |
| `active_connections` 只增不减 | **连接泄漏** |
| `error_total` 狂涨 | 先当正确性问题，别急着谈优化 |

再叠加故障注入（CPU `stress-ng`）：若 P99 从 5ms→500ms 且 queue 堆积，说明资源争用下的排队——符合预期恶化；若直接崩/泄漏，才是缺陷。

---

## 7. 建议落地顺序（讲义完整体，非本仓施工单）

1. **第一版**：atomic 计数（请求、错误、活跃连接）  
2. **第二版**：延迟分位 + 队列长度 + EventLoop 积压  
3. **第三版**：`GET /metrics`  
4. **第四版**：再谈 Prometheus 抓取与 Grafana  

本仓现状：wrk 输出、`test_report` 等是**快照级**观测，**没有常驻 Metrics 栈**。学习重点是**思想**，实现听口令。

---

## 8. 本章小结

1. Metrics = 埋点记账 + 读取出口，不是偷窥线程。  
2. 用原子/每线程统计，避免统计拖垮热路径。  
3. 与 Benchmark 联读，才能区分「线程池慢 / CPU 满 / 连接泄漏」。  
4. `/metrics` 是生产服务思维的入门形态。

下一章：[09_火焰图.md](09_火焰图.md)
