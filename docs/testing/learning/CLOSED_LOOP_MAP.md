# 稳定性闭环 ↔ 本仓库现状（配套讲义）

> 回答：「Benchmark / 故障注入 / Sanitizer / Metrics / 火焰图 —— 我现在用的是哪些？」  
> 与收尾叙事一致：[../../closeout/CLOSEOUT_NARRATIVE.md](../../closeout/CLOSEOUT_NARRATIVE.md)  
> **知识点全集**：[测试学习综合讲义和笔记/README.md](测试学习综合讲义和笔记/README.md)

---

## 1. 先分清：模糊测试 ≠ 故障注入

| 名称 | 含义 | 典型做法 |
|------|------|----------|
| **模糊测试 Fuzz** | 用随机/变异输入砸**某个组件**（多为 Parser），找崩溃与 Sanitizer 问题 | `http_parser_fuzz` + corpus |
| **故障注入 Fault injection** | 在**运行中的系统**上叠加坏环境：延迟、丢包、限 fd、杀磁盘、制造慢客户端等，常与压测一起看退化 | tc/netem、ulimit、混沌工具等 |

本仓有 **Fuzz**；**没有**成体系的「压测中叠加网络/资源故障」。

黑盒里的畸形请求、超大 body = **协议/输入边界测试**，接近「逻辑故障」，仍**不是**压测期资源故障注入。

---

## 2. 五项对照表（你现在主要用哪些）

| 环节 | 你是否在用 | 本仓落点 | 说明 |
|------|------------|----------|------|
| **Benchmark（wrk）** | **是（主路径）** | `gen_report.sh` 内置简易 wrk；`scripts/benchmark.sh` | 已有 QPS/延迟基线（如 ~3.1 万 QPS） |
| **故障注入** | **否（未成体系）** | — | 勿与 fuzz 混称 |
| **模糊测试** | **是（正确性侧）** | `fuzz/`、`gen_report` Phase fuzz | 证明 Parser 韧性，通常不叠在 wrk 上 |
| **Sanitizer（ASan/TSan）** | **能力有、收尾默认未强制** | `run_tests.sh -DWEBSERVER_ENABLE_ASAN=…` 等 | 适合单独 Debug 构建；不宜当作「压测标配」 |
| **Metrics** | **轻量、一次性** | wrk 输出、`test_report_*.md`、summary.tsv | 有观测快照；无常驻监控栈 |
| **火焰图** | **否（未做必选项）** | 文档提及 | P99/CPU 异常时再采 |

### 一句话

你当前实际闭环是：

```text
单元 + 黑盒 +（fuzz）  →  正确性
        ↓
   wrk Benchmark（附带延迟指标）→ 容量基线
```

尚未闭合：`压测 × 故障注入 × Sanitizer × 常驻 Metrics × 火焰图`。

---

## 3. 和「四层测试」怎么拼

```text
单元 / 黑盒 / fuzz     ← 对不对、边界在不在
压测 wrk               ← 能多快、尾巴多长（轻量 metrics）
Sanitizer              ← 内存/竞争（可选加深）
故障注入 + 火焰图      ← 收尾后按需，非本阶段必做
```

---

## 4. 收尾期建议态度

- **保持**：wrk 基线 + gen_report 回归。  
- **知道即可**：故障注入、火焰图、常驻 Metrics 的位置。  
- **按需**：怀疑 use-after-free / 数据竞争时再开 ASan 或 TSan。  
- **不要**：为了「凑满闭环名词」在本仓搭混沌平台或改协议栈。  
