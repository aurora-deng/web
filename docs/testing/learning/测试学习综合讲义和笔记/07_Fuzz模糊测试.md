# 07 · Fuzz 模糊测试（详细讲义）

> **Fuzz（模糊测试）**：自动生成大量随机、异常、边界输入，专门找**崩溃和隐藏缺陷**。  
> 它卡在「功能测试」和「故障测试」中间：更偏 **输入健壮性（Input Robustness）**。  
> **不是** Benchmark，也**不是**普通黑盒。

---

## 我对 Fuzz 的理解（笔记）

**核心定义（自己的话）：**

> **Fuzz = 自动生成大量随机/异常/边界输入去砸程序（尤其 Parser），主要看会不会崩溃、会不会被 Sanitizer 抓住——属于输入健壮性测试。**

和黑盒对比（自己钉死）：

```text
黑盒：人设计输入，断言「应该是 200/404」
Fuzz：机器造输入，通常不断言业务对错，只关心「别崩」
```

**为什么盯 Parser：**  
网络输入空间太大，人工用例盖不住；怪 Header、二进制垃圾、超长字段都可能炸解析器。

**最强组合：** Fuzz + ASan → 怪输入负责触发，ASan 负责指到哪一行。

---

## 1. 先放回你的测试地图

```text
GoogleTest     → 逻辑对不对
黑盒           → 用户行为对不对
Benchmark      → 快不快
故障注入       → 坏环境稳不稳
Sanitizer      → 内存/线程仪器
Fuzz           → 怪输入炸不炸（主攻解析层）
```

Fuzz 主要攻击：

```text
HTTP Parser / Header / URL / Buffer 定界
```

因为：**输入来自不可信网络**，输入空间大到人测不完。

---

## 2. 和黑盒的差别（用例子）

### 黑盒

人设计输入，关心业务结果：

```python
GET /  →  assert "200" in response
```

### Fuzz

机器生成输入，常常**不断言业务对不对**，只关心：

```text
程序有没有崩溃？
ASan 有没有报越界/UAF？
```

例如机器可能生成：

```text
GET /@@@####
G\x00ET ...
Host:::::
Content-Length: -1
超长 AAAAA...
```

人不会也不可能手工写完这些。

---

## 3. 为什么特别适合 HttpParser？

Parser 典型形态：

```text
一串字节进来 → 状态机往前走 → 得到 Request 或 NEED_MORE 或 ERROR
```

- 输入是字节，极易自动化  
- 分支多（半包、chunked、非法 token……）  
- 一旦写越界，就是安全问题  

所以优先级：

1. HTTP Parser ⭐⭐⭐⭐⭐  
2. URL / Query  
3. Header（尤其 CL / TE）  
4. Router 动态匹配（次要）  

---

## 4. libFuzzer 是什么？（人话）

不是纯骰子乱扔。它大致是：

```text
喂一个输入
  → 看执行覆盖了哪些代码路径
  → 变异出「更容易走进新分支」的新输入
  → 循环几十万、几百万次
```

叫**覆盖率驱动**的模糊测试。  
第一次只有 `GET` 走进 method 分支；后来自动长出空格、路径、`HTTP/1.1`……逐步把 Parser 内部走深。

---

## 5. 教学参考代码

```cpp
#include <cstddef>
#include <cstdint>
#include <string>
#include "HttpParser.h"  // 按项目修改

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);

    HttpParser parser;
    // 用真实增量 API 更好：feed 进 Buffer 再 parse
    parser.parse(input);

    return 0;  // 必须返回 0；不要在这里写 EXPECT 业务断言
}
```

注意：

- **没有** `EXPECT_EQ`：Fuzz 目标是「别崩」，不是「这个随机串必须是 200」。  
- 非法输入导致 `PARSE_ERROR` **很正常**，不算失败；**Sanitizer 报错 / 崩溃**才算抓到虫子。  
- 要防「parse 声称 OK 但没消费字节」导致死循环（本仓速查里提过）。

本仓路径：`fuzz/` + `gen_report` 的 fuzz 阶段。

---

## 6. 最强组合：Fuzz + ASan

```text
Fuzz 负责：找到奇怪输入
ASan 负责：告诉你哪一行堆溢出/UAF
```

例如变异出天文数字 `Content-Length` → Buffer 错误扩容 → ASan：`heap-buffer-overflow` at `HttpParser.cpp:80`。

---

## 7. 对照表（再背一次）

| 测试 | 目标 |
|------|------|
| GoogleTest | 逻辑正确 |
| 黑盒 | 协议行为 |
| Benchmark | 性能 |
| Fault Injection | 异常环境 |
| Fuzz | 异常输入 |
| Sanitizer | 给运行中的错误装探头 |

它们是**组合拳**，不是互相替代。

---

## 8. 本章小结

1. Fuzz = 自动砸输入，找崩与 Sanitizer 问题。  
2. 主攻 Parser；与黑盒「人对手写用例」互补。  
3. 和 ASan 一起用才完整。  
4. 本仓已有 Fuzz；它证明**解析韧性**，通常不叠在 wrk 压测上。

下一章：[08_Metrics监控.md](08_Metrics监控.md)
