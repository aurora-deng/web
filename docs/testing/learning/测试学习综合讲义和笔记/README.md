# 测试学习综合讲义和笔记

> **标记**：第四阶段 · 高性能服务器测试与稳定性工程（**详细讲义 + 笔记**）  
> **写法**：每一层都按「是什么 → 为什么 → 怎么想 → 怎么用 → 样例 → 和本仓关系」展开，尽量白话、好懂。  
> **目标**：把第三阶段 WebServer 从「能跑」提升到「可验证、可优化、可维护」。  
> **纪律**：讲清原理即可；是否实现 Metrics/故障平台听口令。现状对照：[../CLOSED_LOOP_MAP.md](../CLOSED_LOOP_MAP.md)。

---

## 用一张图先记住闭环

```text
代码实现
   │
单元测试验证组件正确性          ← GoogleTest（零件对不对）
   │
黑盒测试验证服务行为            ← Python TCP（用户看的对不对）
   │
Sanitizer 发现隐藏 Bug          ← ASan / UBSan / TSan
   │
Benchmark 发现性能瓶颈          ← wrk（快不快、扛多少）
   │
Metrics 观察系统状态            ← 埋点 /metrics（肚子里怎样）
   │
perf / 火焰图定位优化点         ← CPU 花在哪
   │
Fuzz 提高健壮性                 ← libFuzzer（怪输入炸不炸）
```

升级后的系统观：

```text
              Benchmark
                 │
Client -------- WebServer -------- Metrics
                 │
          Fault Injection
                 │
       ASan / TSan / UBSan
                 │
            FlameGraph / Fuzz
```

关键词：**Performance + Reliability Engineering**（性能工程 + 可靠性工程）。

---

## 阅读顺序（请按序读，每章都是「详细版」）

| 章 | 文件 | 你读完应能说清 |
|----|------|----------------|
| ★ | [我对各层测试的理解.md](我对各层测试的理解.md) | **你自己的一句话理解汇总（先读/常复习）** |
| 00 | [00_总览与测试分层.md](00_总览与测试分层.md) | 为什么分层；单元 vs 调试；每层一句话职责 |
| 01 | [01_GoogleTest单元测试.md](01_GoogleTest单元测试.md) | 怎么测 Buffer/Parser；断言代替人工看输出 |
| 02 | [02_EventLoop与生命周期测试.md](02_EventLoop与生命周期测试.md) | 为什么 Reactor 难测；pipe/假事件；生命周期 |
| 03 | [03_HTTP黑盒测试.md](03_HTTP黑盒测试.md) | 假客户端；半包粘包 Keep-Alive；黑盒≠压测 |
| 04 | [04_Benchmark压测.md](04_Benchmark压测.md) | QPS/P99；基线；对比实验怎么设计 |
| 05 | [05_Sanitizer.md](05_Sanitizer.md) | ASan/TSan 各查什么；为何分构建；高危点 |
| 06 | [06_故障注入.md](06_故障注入.md) | 与 Fuzz 区别；慢客户端；tc/stress |
| 07 | [07_Fuzz模糊测试.md](07_Fuzz模糊测试.md) | 覆盖率驱动；只关心崩不崩 |
| 08 | [08_Metrics监控.md](08_Metrics监控.md) | 埋点不是偷窥线程；如何联读压测 |
| 09 | [09_火焰图.md](09_火焰图.md) | 宽度=热点；perf 流程；典型读图 |
| ★ | [火焰图生成命令流与读图教学.md](火焰图生成命令流与读图教学.md) | **命令流 + 读图教学 + 本机火焰图实战解读** |
| 10 | [10_工程化流水线与验收.md](10_工程化流水线与验收.md) | CI 思想；验收清单；简历表述 |

每章开头都有 **「我对…的理解（笔记）」**，用你的话钉死定义；正文再展开原理与用法。

---

## 和其它文档怎么配合

| 材料 | 用途 |
|------|------|
| **本讲义** | 把「为什么、怎么想」讲透（主教材） |
| [../学习笔记速查.md](../学习笔记速查.md) | 本仓**已有用例名单**速查 |
| [../CLOSED_LOOP_MAP.md](../CLOSED_LOOP_MAP.md) | 讲义闭环 vs **本仓已落地** |
| [../ROUND1_INVARIANTS.md](../ROUND1_INVARIANTS.md) | 练习：不变量 ↔ 用例 |
| [../../TESTING.md](../../TESTING.md) | 怎么跑测试 |
| [../../../performance/BENCHMARK.md](../../../performance/BENCHMARK.md) | 可复现压测协议 |

---

## 口令

见 [../../LEARNING_SCOPE.md](../../LEARNING_SCOPE.md)。  
读讲义 ≠ 立刻改 `server/**` 或搭监控/混沌平台。
