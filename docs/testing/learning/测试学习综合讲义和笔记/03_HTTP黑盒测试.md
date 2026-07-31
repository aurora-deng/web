# 03 · HTTP 黑盒测试（详细讲义）

> **黑盒测试（Black Box Testing）**  
> = 站在「用户 / 客户端」角度，**不打开机箱**，只通过真实网络协议跟服务器交互，看行为是否符合预期。  
> 你的理解「用 Python 连 IP:端口发数据，模拟人工访问」——**方向完全正确**。下面把它讲透，并分清和 Apifox、Benchmark 的差别。

---

## 我对黑盒测试的理解（笔记）

**核心定义（自己的话）：**

> **黑盒测试 = 站在用户/客户端角度，不关心服务器内部实现，通过真实网络协议与服务器交互，验证服务行为是否符合预期。**

更直白：

> 用 Python 连上 IP 和端口发 HTTP，其实就是**写程序模拟人工/浏览器访问**；不打开机箱，只看发了啥、收回啥。

和 Apifox 很像：都是按**契约**发包再检查响应。  
差别是：我测的是自研 TCP+HTTP，所以更常用**裸 socket**，好测半包、粘包、Keep-Alive、异常断开。

**契约从哪来（自己怎么保证「发的东西对」）：**

1. 遵守 HTTP/1.1 标准写法（请求行、Header、空行）  
2. 按本服务器实际支持的功能测  
3. 故意发错误输入，看服务器稳不稳——**验证的是服务器，不是验证 Python**

**和单元 / 压测的分工：**

```text
单元：零件对不对
黑盒：整机行为对不对（用户视角）
Benchmark：压力下快不快（不是黑盒的放大版那么简单）
```

---

## 1. 黑盒在测什么？用浏览器类比

真实用户：

```text
Chrome
  │ connect()
  │
127.0.0.1:8080
  │
你的 WebServer
  │
返回 HTTP/1.1 200 ...
```

浏览器做的三步：

1. TCP `connect`  
2. 发送 HTTP 文本  
3. `recv` 响应  

Python 黑盒就是**换掉浏览器**，你自己当客户端：

```text
Python 程序
  │ socket
  │
你的 WebServer
```

```python
sock.connect(("127.0.0.1", 8080))
sock.send(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
response = sock.recv(4096)
```

本质与浏览器相同，但你**完全控制发送内容**。

---

## 2. 和单元测试的区别（白盒 vs 黑盒）

### 单元测试（偏白盒）

你知道内部有 Buffer、Parser、Router，直接调：

```cpp
parser.parse(...)
EXPECT_EQ(...)
```

### 黑盒

你假装什么都不知道，只知道：

```text
输入（HTTP 请求字节）
   │
服务器（黑盒子）
   │
输出（HTTP 响应字节）
```

| | 单元 | 黑盒 |
|--|------|------|
| 视角 | 开发者 / 零件 | 用户 / 整机 |
| 典型工具 | GoogleTest | Python socket |
| 能发现 | 函数逻辑错 | accept 错、漏写响应、Keep-Alive 错、整链半包错 |
| 发现不了 | 整机装配问题 | 某个私有函数内部小分支（除非表现为错误响应） |

所以：**零件全绿，整机仍可能挂**——这就是黑盒存在的理由。

---

## 3. 为什么不用浏览器点一点？

浏览器太「聪明」：

- 自动加 Header、缓存、Cookie  
- 可能多连接、重定向、帮你容错  

你看到页面正常，**分不清**是服务器对了，还是浏览器帮你圆了。  
而且浏览器**不会**按你要求发送：

```http
GETTTTT /abc HTTP/1.1
```

黑盒可以。底层服务器测试：**越接近裸 socket 越好**。

---

## 4. 「Python 发的数据符合服务器吗？」——三层保障

这很像 Apifox：先有**契约**，再按契约发包。

### 第一层：遵守 HTTP/1.1 标准

请求必须长这样（结构）：

```text
请求行：METHOD PATH VERSION\r\n
头部：  Key: Value\r\n
空行：  \r\n
正文：  （由 Content-Length 等决定）
```

所以：

```python
request = (
    "GET / HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "\r\n"
)
```

不是随便打字，是**按协议拼**。

### 第二层：按你服务器实际支持的功能测

服务器宣称支持：`GET /`、Keep-Alive、某条路由 → 黑盒就覆盖这些。  
不支持的特性（例如某种 chunked 变体）不要当成「正常成功路径」去强断言。

### 第三层：故意发错误输入

黑盒不只测「快乐路径」，也测「服务器被虐时会不会崩」：

```http
GETTTTT / HTTP/1.1
```

期望：400 或断开（按你框架策略），**不能进程崩溃**。

重点：

> 黑盒验证的是**服务器**；Python 只是工具。换成 Go/Java 客户端也一样。

---

## 5. 黑盒的三个层级（由浅入深）

### Level 1：HTTP 功能（像 Apifox）

- GET `/` → 200  
- 不存在路径 → 404  
- POST + Body → 正文被正确处理  

### Level 2：TCP 行为（你的重点）

| 场景 | 含义 | 服务器应如何 |
|------|------|--------------|
| **半包** | 一次只发 `GET /`，隔一会再发剩下的 | Buffer 拼接，拼完再响应 |
| **粘包 / 流水线** | 一次 send 两个完整请求 | 解析出两个请求（顺序响应） |
| **异常断开** | send 后直接 close | 释放 Connection，不泄漏、不崩 |
| **Keep-Alive** | 同一 TCP 上下两个请求 | 连接保持，两次都 200 |

### Level 3：并发功能测试（仍不是 Benchmark）

100 个 Python 线程同时 GET——验证「大致并发下别崩、别串响应」。  
Python 慢，**测不出**工业 QPS；那是 wrk 的事。

---

## 6. 最小可运行示例（教学参考）

路径习惯：`tests/integration/http_blackbox.py`

```python
import socket

HOST, PORT = "127.0.0.1", 8080

def send_request(request: str) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))
    sock.sendall(request.encode())
    response = sock.recv(4096)
    sock.close()
    return response.decode(errors="replace")

def assert_contains(response: str, keyword: str):
    assert keyword in response, response

def test_get_root():
    req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
    resp = send_request(req)
    assert_contains(resp, "200")

def test_404():
    req = "GET /not_exist HTTP/1.1\r\nHost: localhost\r\n\r\n"
    assert_contains(send_request(req), "404")

if __name__ == "__main__":
    test_get_root()
    test_404()
    print("ok")
```

运行：先起 `./server`（或本仓由 CTest 自动拉起），再跑脚本。  
用 `assert` 而不是只 `print`——这才叫**自动化验收**。

---

## 7. Keep-Alive 为什么特别重要？

HTTP/1.1 默认倾向长连接：

```text
同一条 TCP：
  请求1 → 响应1
  请求2 → 响应2
  （不断开）
```

这直接锻炼你的：

- Connection 状态机  
- Buffer 是否清空/复用正确  
- Parser 是否能连续解析多个请求  

```python
def test_keep_alive():
    sock = socket.create_connection(("127.0.0.1", 8080))
    sock.sendall(
        b"GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
    )
    r1 = sock.recv(8192)
    sock.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    r2 = sock.recv(8192)
    assert b"200" in r1 and b"200" in r2
    sock.close()
```

---

## 8. 半包 / 粘包（和单测的差别）

单测半包：直接 `parser.feed("GET /")` 再 `feed(剩余)`。  
黑盒半包：走**真 socket**，中间还可以 `sleep`，更接近网络抖动。

```python
import time
sock.send(b"GET /")
time.sleep(0.1)
sock.send(b" HTTP/1.1\r\nHost: x\r\n\r\n")
```

粘包 / 流水线：

```python
sock.sendall(req1_bytes + req2_bytes)
# 再按你的读策略收齐两个响应
```

---

## 9. 建议覆盖清单

| 测试 | 目的（人话） |
|------|----------------|
| GET | 整条链路通不通 |
| POST | Body / Content-Length 对不对 |
| 404 | 路由失败行为 |
| Keep-Alive | 连接复用 |
| 半包 | 增量解析 |
| 大 Body / 上限 | Buffer 与拒绝策略 |
| 畸形请求 | 会不会崩 |
| `/slow` + `/fast` | 慢业务堵不停 IO（Executor） |
| 多连接并发 | 粗并发正确性 |

本仓具体场景名见：[../学习笔记速查.md](../学习笔记速查.md)。

---

## 10. 黑盒 vs Benchmark（最容易混）

| | 黑盒 | Benchmark |
|--|------|-----------|
| 问句 | **功能对不对？** | **性能怎么样？** |
| 规模 | 几个到几百连接试行为 | 成千上万连接持续砸 |
| 工具 | Python socket | wrk |
| 成功标准 | 状态码/正文/连接行为正确 | QPS、P99、错误率≈0 |

错误顺序：一上来万级 wrk，错误率 20%，却说不清是协议错还是性能崩。  
正确顺序：**单元 → 黑盒 → Benchmark**。

---

## 11. 本章小结（背四句）

1. 黑盒 = 假客户端 + 真协议 + 断言可观察行为。  
2. 契约来自 HTTP 标准 + 你的服务器设计 + 故意错误输入。  
3. 对你这种自研 TCP 服务器，**TCP 半包/粘包/Keep-Alive/断开**比「页面好不好看」更重要。  
4. 黑盒证明「机器行为对」；下一章 Benchmark 证明「机器能多快」。

下一章：[04_Benchmark压测.md](04_Benchmark压测.md)
