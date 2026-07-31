# 01 · GoogleTest 单元测试（详细讲义）

> **目标**：测试服务器的**内部组件**，不是整个服务器。  
> **思维方式**：预先定义「这个零件收到什么输入，必须吐出什么结果」，然后让机器自动验证。

---

## 我对 GoogleTest / 单元测试的理解（笔记）

**核心定义（自己的话）：**

> **单元测试 = 预先定义「程序应该产生什么行为」，然后自动验证实际行为是否符合预期。**

可以先粗浅理解成：

```text
我先想好：输入 A，输出必须是 B
跑程序
机器用 EXPECT 判断：实际是不是 B
```

也就是从：

```text
cout << 结果;   → 人眼看对不对
```

升级到：

```text
EXPECT_EQ(实际, 期望);  → 机器自动判
```

**和断点调试的关系（容易混，这里钉死）：**

| | 单元测试 | 断点调试 |
|--|----------|----------|
| 像在问 | **以后还会不会再错？**（回归） | **这次为什么错了？**（定位） |
| 谁在看 | 机器按预设期望检查 | 人看变量和调用栈 |
| 共同点 | 都是「给输入 → 看输出 → 判断对不对」 | 同左 |

所以：方向「预判输出再验证」是对的；更完整的说法是 **自动化、可重复的行为契约检查**，不是替代调试，而是和调试分工。

**测的范围：** 内部零件（Buffer / Parser / Router…），不是整台服务器的 accept/epoll 全链路。

---

## 1. 单元测试在测什么？用白话说

你的项目里有很多「零件」：

```text
src/
  Buffer/
  HTTP/HttpParser
  Router/
  Timer/
  ThreadPool / Executor
```

单元测试就是：

```text
拿出一个零件
  → 给它固定输入
  → 看输出是不是你规定的样子
  → 对了 PASS，错了 FAIL（并打印期望 vs 实际）
```

它**不知道**也不关心：

```text
socket / accept / epoll / 真实端口 8080
```

所以：Parser 测通了，**不代表**整机会把响应发出去。那是黑盒的事。

---

## 2. 为什么先做单元测试？

因为零件错了，整机一定错；而且零件测：

- **快**（毫秒级，不用起服务器）  
- **定位准**（失败直接指向 Buffer/Parser 某条断言）  
- **可回归**（你以后改优化，一跑就知道有没有弄坏旧行为）

---

## 3. 引入 GoogleTest（CMake 教学参考）

目录习惯：

```text
tests/
  CMakeLists.txt
  unit_tests.cpp
```

教学用 CMake：

```cmake
enable_testing()
find_package(GTest REQUIRED)

add_executable(unit_tests unit_tests.cpp)

target_link_libraries(unit_tests
  PRIVATE
  GTest::gtest
  GTest::gtest_main
  # 链上你的 server 库目标
)

add_test(NAME unit_test COMMAND unit_tests)
```

本仓实际用法：`-DBUILD_TESTING=ON` + 系统安装 `libgtest-dev`，详见 [../../TESTING.md](../../TESTING.md)。一般用：

```bash
bash scripts/run_tests.sh
```

成功时你会看到类似：

```text
100% tests passed
```

---

## 4. 第一个测试：Buffer（最该吃透）

Buffer 是「字节容器」：网络读进来的数据先躺在这里，Parser 再从里面啃。

常见能力：

```text
append()     往里追加
retrieve()   取走已消费部分
size()       当前可读长度
find CRLF    找行结束（HTTP 很依赖）
```

### 教学样例

```cpp
#include <gtest/gtest.h>
#include "Buffer.h"

TEST(BufferTest, AppendRead) {
    Buffer buf;

    buf.append("hello");

    EXPECT_EQ(buf.size(), 5u);

    std::string data = buf.retrieveAll();  // 按你项目真实 API 改名
    EXPECT_EQ(data, "hello");
}
```

### 这段在证明什么？

1. 写进去的字节数对；  
2. 读出来内容没丢、没乱。

### 你还应该想到的边界（笔记）

- 小容量反复 append → 触发**扩容 / 压缩**，数据不能丢  
- 查找 `\r\n`、`\r\n\r\n` 是否正确  
- 取走一半后，剩下的是否还正确  

本仓已有用例名可对照：`BufferTest.AppendsCompactsAndFindsDelimiters`（见[速查](../学习笔记速查.md)）。

---

## 5. 第二个测试：HTTP Parser（核心）

### 输入是一段「假 HTTP」

```http
GET /hello HTTP/1.1\r\n
Host: test.com\r\n
\r\n
```

### 你预先规定的行为

```text
method == "GET"
path   == "/hello"
Host   == "test.com"（若你解析了 headers）
```

### 教学样例

```cpp
TEST(HttpParserTest, ParseGet) {
    HttpParser parser;

    std::string request =
        "GET /hello HTTP/1.1\r\n"
        "Host: test.com\r\n"
        "\r\n";

    // 注意：你的真实 API 可能是对 Buffer 增量 feed，而不是一次 parse(string)
    auto result = parser.parse(request);

    EXPECT_EQ(result.method, "GET");
    EXPECT_EQ(result.path, "/hello");
}
```

### 为什么「整串一次 parse」不够？

真实 TCP 经常是：

```text
第一次 recv:  "GET /hel"
第二次 recv:  "lo HTTP/1.1\r\nHost:...\r\n\r\n"
```

Parser 必须：**第一次说还不够（NEED_MORE），状态保留；第二次拼完再成功**。  
所以工业级单测会写「半包」——本仓就有 `WaitsForHalfPacketAndContentLengthBody`。

### 失败时怎么用？

```text
FAILED: path 期望 /index.html 实际 /index.htm
  → 这时才开调试器进 parseRequestLine
  → 修完后测试变绿
  → 测试留下防回归
```

---

## 6. Router 测什么？（白话）

Router 负责：「这个路径交给哪个 handler」。

至少三类行为：

| 行为 | 你在断言什么 |
|------|----------------|
| 动态路由 `/user/:id` | 参数 `id` 提取得对 |
| 中间件短路 | 中间件不调 `next` 时，后面 handler 不能执行 |
| 404 | 无路由时状态码/响应体符合约定 |

**易错点（本仓踩过）**：有的实现会 `swap` 响应对象，断言如果盯着「栈上临时 HttpResponse」会测错对象。应断言**最终真正发出去的那份**（如 context 上的 response）。

---

## 7. 覆盖目标（方向，不是死 KPI）

| 模块 | 建议吃透程度 | 原因 |
|------|--------------|------|
| Buffer | 极高 | 一切字节的地基 |
| Parser | 高 | 协议正确性中枢 |
| Router | 高 | 业务分发 |
| Timer | 中高 | 超时与生命周期 |
| ThreadPool | 中高 | 并发基础 |

---

## 8. 运行方式（笔记）

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

或一键：`bash scripts/run_tests.sh`。

---

## 9. 本章小结（背三句）

1. 单元测试测**零件**，预定义行为，自动断言。  
2. 它和调试目的不同：调试找「为什么」，测试防「再错」。  
3. Buffer / Parser 是 C++ 网络服务器单测的重中之重；测完再进 EventLoop 生命周期（下一章）。

下一章：[02_EventLoop与生命周期测试.md](02_EventLoop与生命周期测试.md)
