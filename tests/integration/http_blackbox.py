#!/usr/bin/env python3
"""
==============================================================================
Linux HTTP 黑盒回归测试脚本（面向自研C++协程HTTP框架CI自动化测试）
核心定位：黑盒测试，零侵入待测服务
【核心能力清单】
1. 自动拉起待测HTTP服务二进制程序作为独立进程组管理
2. 仅使用原生socket TCP通信，不导入requests、http.client等任何HTTP高层库
3. 手动实现标准HTTP/1.1协议解析，完全模拟真实客户端TCP分片、粘包场景
4. 覆盖全套框架核心模块验证：事件循环调度、HTTP请求解析器、路由分发、中间件、
   TCP长连接管理、KeepAlive复用、HTTP流水线Pipeline、Chunked流式编码、
   静态资源Range分片、ETag缓存、并发阻塞隔离、非法报文安全防御
【对比单元测试巨大优势】
单元测试直接调用代码内部接口，绕过真实TCP缓冲区、分包、操作系统内核行为；
黑盒测试从外部发包，可以稳定捕获线上高频致命缺陷：
    粘包半包解析残留、长连接缓冲区串包污染、文件描述符泄漏、请求走私漏洞、
    Reactor主线程被业务阻塞、异常连接未回收、超大报文溢出等。
【运行环境】Linux；Python3.7+
【CI启动命令示例】
python3 http_blackbox_test.py --server ./bin/http_server
==============================================================================
"""

# 启用Python新式返回类型注解（3.7需要导入，3.10+原生支持）
from __future__ import annotations

import argparse
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import threading
from pathlib import Path

# ===================== 全局基础配置（统一修改入口） =====================
# 待测服务监听回环地址，禁止使用0.0.0.0避免外部网络干扰测试结果
HOST = "127.0.0.1"
# HTTP服务固定监听端口，待测框架必须监听8080
PORT = 8080
# Socket读写超时阈值
# 关键作用：一旦服务死锁、挂死、无响应，测试脚本不会无限阻塞卡住CI流水线
TIMEOUT = 3.0


def require(condition: bool, message: object) -> None:
    """
    自定义测试断言函数，替代原生assert
    【为什么不用原生assert】
    Python解释器-O优化模式下会直接删除所有assert语句，造成测试失效；
    自定义断言永久生效，并且统一异常格式，附带详细报文信息便于日志排查。
    :param condition: 布尔判断条件，True代表测试通过
    :param message: 失败时打印调试信息，支持原始字节报文、字符串、结构体
    """
    if not condition:
        raise AssertionError(message)


class HttpConnection:
    """
    原生TCP连接封装客户端
    完全自主实现HTTP/1.1解析，不依赖任何官方HTTP库
    支持特性：
    1. Keep-Alive长连接复用，单连接连续发送多条请求
    2. Content-Length定长响应体解析
    3. Transfer-Encoding: chunked 标准分块流式解码（包含Trailer处理）
    4. HTTP流水线Pipeline：一次性批量发送多条请求，随后逐条读取响应
    设计核心目标：模拟操作系统TCP流式缓冲区，复现粘包、半包线上场景
    """

    def __init__(self) -> None:
        """
        构造函数：建立TCP连接完成三次握手
        create_connection为高层封装，内部调用socket connect，自动处理IPv4/IPv6
        """
        # 建立TCP连接，设置连接阶段超时
        self.socket = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        # 设置socket读写超时，recv/send阻塞超过时间直接抛出异常
        self.socket.settimeout(TIMEOUT)
        """
        socket.makefile("rb") 创建二进制缓冲读流，本脚本最关键设计点之一
        特性说明：
        1. 内置缓冲区，数据读到内存缓存，支持readline()按 \r\n 分割HTTP文本行；
        2. 整个长连接生命周期共用同一个reader缓冲区；
        3. 可以完美复现一个重大BUG场景：上一条响应剩余字节残留在缓冲区，污染下一条请求解析；
        普通单次短连接测试无法发现该类长连接缺陷。
        """
        self.reader = self.socket.makefile("rb")

    def close(self) -> None:
        """
        有序释放TCP连接资源
        释放顺序强制要求：先关闭缓冲reader，再关闭底层socket fd
        风险：顺序颠倒会导致缓冲区残留数据无法释放，产生文件描述符泄漏
        """
        self.reader.close()
        self.socket.close()

    def request(self, raw_request: bytes) -> tuple[int, dict[str, str], bytes]:
        """
        发送单条完整原始HTTP二进制请求报文，并阻塞等待完整响应
        :param raw_request: 原始二进制HTTP报文
        :return: 三元组 (HTTP状态码int, 规范化头部字典, 响应body字节流)
        """
        # sendall：持续发送直到全部数据写完，自动循环处理TCP分段发送，不会半写
        self.socket.sendall(raw_request)
        # 调用解析函数读取一份完整响应
        return self.read_response()

    def read_response(self) -> tuple[int, dict[str, str], bytes]:
        """
        从当前连接缓冲区读取**单个完整HTTP响应报文**
        独立抽离成函数：适配Pipeline流水线场景，批量发送请求后循环调用多次解析响应
        遵循RFC2616标准解析流程：状态行 -> 响应首部 -> 空行分隔 -> 响应体
        """
        # 读取第一行：HTTP状态行 例：b"HTTP/1.1 200 OK\r\n"
        status_line = self.reader.readline()
        # 强制校验协议版本，本脚本仅兼容HTTP/1.1
        require(status_line.startswith(b"HTTP/1.1 "), f"非法状态行：{status_line}")
        # 分割字符串，最多分割两段，取出中间状态码
        status = int(status_line.split(b" ", 2)[1])

        headers: dict[str, str] = {}
        # 循环持续读取响应头部，直到读取到单独 \r\n（HTTP规定首部结束标记）
        while True:
            line = self.reader.readline()
            # 连接提前关闭、数据流截断属于协议异常
            require(bool(line), "读取HTTP首部过程中连接被服务端提前关闭")
            if line == b"\r\n":
                break
            # HTTP协议规定首部名称大小写不敏感，统一转为小写消除测试结果不稳定问题
            # latin1编码：一字节映射无损转换二进制数据；utf8遇到非法字节直接抛出异常，不适合原始报文解析
            name, value = line.decode("latin1").split(":", 1)
            headers[name.lower()] = value.strip()

        # ========== 两种标准响应体解析分支，RFC规定互斥 ==========
        # 优先级：chunked高于Content-Length，服务端不能同时下发
        if headers.get("transfer-encoding", "").lower() == "chunked":
            body = self._read_chunked()
        else:
            # 若无Content-Length默认长度0
            length = int(headers.get("content-length", "0"))
            body = self.reader.read(length)
            # 强校验读取字节数量，防止服务端返回半包、提前关闭连接导致数据截断
            require(len(body) == length, f"响应body长度不足，预期:{length} 实际:{len(body)}")
        return status, headers, body

    def _read_chunked(self) -> bytes:
        """
        HTTP/1.1 chunked分块编码完整解码器（自研框架高频出错模块）
        RFC规范格式：
            [十六进制块大小]\r\n
            [块二进制数据]\r\n
            循环上述结构
            0\r\n
            [可选Trailer首部]\r\n
        极易踩坑点：
        1. 解析完size=0后直接退出，没有读取Trailer区域；
        2. 缺失块数据末尾强制分隔符 \r\n；
        上述错误直接导致缓冲区残留字节，长连接下一次请求解析错乱。
        """
        chunks: list[bytes] = []
        while True:
            # 读取单行块大小
            size_line = self.reader.readline()
            require(size_line.endswith(b"\r\n"), f"chunk块大小行格式错误：{size_line}")
            # ; 后面属于chunk扩展参数，协议允许忽略，直接截断丢弃
            size_hex = size_line.split(b";", 1)[0]
            # 将十六进制字符串转为十进制字节长度
            size = int(size_hex, 16)
            # size等于0代表所有分块数据传输完成
            if size == 0:
                # !!!!极其关键逻辑：必须完整消费所有Trailer行直到空行!!!!
                # 如果跳过Trailer读取，缓冲区内遗留数据会污染同连接下一条请求
                while True:
                    trailer = self.reader.readline()
                    require(bool(trailer), "读取chunk trailer头部时连接异常断开")
                    if trailer == b"\r\n":
                        break
                # 拼接所有分块数据返回完整body
                return b"".join(chunks)
            # 读取指定长度二进制块内容
            chunk = self.reader.read(size)
            require(len(chunk) == size, f"chunk数据缺失，预期{size}字节")
            # 每个数据块之后强制跟随 \r\n 分隔符，必须读取并消费
            require(self.reader.read(2) == b"\r\n", "chunk数据包缺少结尾CRLF分隔符")
            chunks.append(chunk)


def reclaim_port_for_server(server: Path) -> None:
    """
    端口清理工具函数
    使用场景：上一轮测试异常崩溃，待测服务僵尸进程未正常退出，持续占用8080端口
    执行策略：
    1. 优先调用项目内置端口清理脚本（如果存在）；
    2. 不存在辅助脚本，则通过pkill信号终止包含程序路径的进程；
    3. 循环等待端口释放，最大等待3秒
    """
    # 约定项目目录层级：二进制位于 /bin，脚本目录 /scripts
    root = server.resolve().parent.parent
    helper = root / "scripts" / "free_port_8080.py"
    if helper.is_file():
        subprocess.run(
            [sys.executable, str(helper), str(server.resolve())],
            check=False,
        )
        return
    # 无辅助清理脚本，使用进程名匹配杀死旧实例
    proc_hint = str(server.resolve())
    # 先发送优雅退出信号，超时再强制杀死
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            subprocess.run(
                ["pkill", f"-{int(sig)}", "-f", proc_hint],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError:
            pass
        # 轮询检测端口状态，直到端口空闲或超时
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if not _has_tcp_listen(PORT):
                return
            time.sleep(0.1)


def _has_tcp_listen(port: int) -> bool:
    """
    Linux内核端口检测底层函数
    直接读取 /proc/net/tcp、/proc/net/tcp6，不依赖ss、netstat命令
    适用极简docker容器环境（很多精简镜像不预装网络工具）
    Linux内核TCP状态编码：0A = LISTEN（正在监听端口）
    注意：TIME_WAIT、ESTABLISHED属于已建立连接，不判定为端口占用
    端口在内核文件中以十六进制存储，需要进制转换。
    """
    for proc_path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            # 读取内核文件，第一行为表头直接丢弃
            lines = open(proc_path, encoding="utf-8").read().splitlines()[1:]
        except OSError:
            # 容器权限不足无法读取/proc，直接跳过本轮检测
            continue
        for line in lines:
            parts = line.split()
            # 第4列为连接状态字段st
            if len(parts) < 10 or parts[3] != "0A":
                continue
            try:
                # local_address格式示例：0100007F:1F90  ip十六进制:端口十六进制
                port_hex = parts[1].rsplit(":", 1)[-1]
                local_port = int(port_hex, 16)
            except ValueError:
                continue
            if local_port == port:
                # 找到处于监听状态的socket，端口被占用
                return True
    return False


def assert_port_available() -> None:
    """
    服务启动前置强校验
    如果端口存在LISTEN监听进程，直接抛出异常终止测试
    备注：SO_REUSEADDR只能解决进程快速重启TIME_WAIT问题，无法处理正在运行的监听进程
    """
    if _has_tcp_listen(PORT):
        raise RuntimeError(f"{HOST}:{PORT} 已有进程处于LISTEN监听状态，无法启动服务")


def wait_until_ready(process: subprocess.Popen[bytes]) -> None:
    """
    阻塞等待待测服务初始化完成
    实现方式：循环建立短TCP连接探测端口，不发送HTTP报文，干净无污染
    双重监控：
    1. 持续检测进程是否意外崩溃退出；
    2. 检测端口是否开始监听；
    最大等待10秒，防止服务初始化死锁导致测试卡死
    """
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        # poll() != None 代表进程已经退出
        if process.poll() is not None:
            raise RuntimeError(f"服务启动阶段异常退出，返回码:{process.returncode}")
        try:
            # 尝试TCP握手，握手成功代表服务listen完成
            with socket.create_connection((HOST, PORT), timeout=0.2):
                return
        except OSError:
            # 连接拒绝说明端口未就绪，短暂休眠重试
            time.sleep(0.05)
    raise RuntimeError("10秒内服务未监听8080端口，启动初始化超时")


def expect_connection_closed(payload: bytes) -> None:
    """
    HTTP安全防护测试专用函数
    向服务发送畸形、非法、违规HTTP报文，校验安全策略是否生效
    【符合安全规范的两种行为】
    1. 收到非法报文直接发送RST重置TCP连接；
    2. 返回4xx错误响应，随后主动关闭连接；
    【高危漏洞行为（测试失败）】
    正常处理非法报文，返回HTTP/1.1 200 OK，极易引发HTTP请求走私攻击
    :param payload: 畸形HTTP原始二进制报文
    """
    sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    try:
        try:
            sock.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            # 写入阶段直接被断开，符合安全预期，测试通过
            return
        received = bytearray()
        # 持续循环读取，直到连接关闭
        while True:
            try:
                part = sock.recv(4096)
            except ConnectionResetError:
                return
            if not part:
                break
            received.extend(part)
        # 禁止非法报文返回成功响应
        require(
            not received.startswith(b"HTTP/1.1 200"),
            f"高危缺陷：非法报文被正常处理！收到响应头部：{bytes(received[:128])}"
        )
    finally:
        sock.close()


def run_checks() -> None:
    """
    所有回归测试用例入口
    用例设计原则：由基础功能 → 复杂协议特性 → 安全测试 → 架构核心验证
    每条用例对应待测服务约定路由，框架需要预先实现对应接口：
        /、/user/{id}、/admin、/stream1、/fast、/slow、/logo(静态资源)、/large(超大稀疏文件)
    """
    # ===================== 用例1：Keep-Alive基础验证，同一条TCP连接连续两次请求 =====================
    # 测试目标：验证长连接缓冲区正确清理，两次请求解析互不干扰，无粘包串包
    connection = HttpConnection()
    try:
        status, headers, body = connection.request(
            b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        require(status == 200, f"根路由状态码异常: {status}")
        require(body == b"<h1>hello</h1>", f"根路由响应内容错误:{body}")
        require(headers.get("connection") == "keep-alive", "服务未返回keep-alive首部，长连接未开启")

        # 在同一个TCP长连接发起第二条请求
        status, _, body = connection.request(
            b"GET /user/123 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        require(status == 200, f"/user路由状态码异常: {status}")
        require(body == b"123", f"/user路由路径参数解析错误:{body}")
    finally:
        connection.close()

    # ===================== 用例2：中间件权限拦截测试 /admin未认证访问 =====================
    # 测试目标：路由前置中间件能否正常短路请求，提前返回401，不继续执行业务逻辑
    admin = HttpConnection()
    try:
        status, _, body = admin.request(
            b"GET /admin HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        require(status == 401, f"/admin 权限校验中间件失效，状态码:{status}")
        require(body == b"Unauthorized", f"401响应内容错误:{body}")
    finally:
        admin.close()

    # ===================== 用例3：Chunked流式响应编码完整解码测试 =====================
    # 测试目标：Transfer-Encoding: chunked编码发送、解码完整性校验
    stream = HttpConnection()
    try:
        status, headers, body = stream.request(
            b"GET /stream1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        )
        require(status == 200, f"流式接口状态码异常:{status}")
        require(headers.get("transfer-encoding") == "chunked", "流式接口没有启用chunked分块编码")
        require(body == b"hello world", f"chunked解码结果错误:{body}")
    finally:
        stream.close()

    # ===================== 用例4：非法HTTP报文、冲突首部安全防御测试 =====================
    # 场景1：不支持的HTTP协议版本 HTTP/9.9
    expect_connection_closed(b"GET / HTTP/9.9\r\nHost: localhost\r\n\r\n")
    # 场景2：同时携带Content-Length + Transfer-Encoding，RFC禁止，经典HTTP请求走私漏洞触发条件
    expect_connection_closed(
        b"POST / HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Content-Length: 4\r\n"
        b"Transfer-Encoding: chunked\r\n\r\n"
        b"0\r\n\r\n"
    )

    # ===================== 用例5：超大请求体限制防护测试 =====================
    # 构造超出服务配置最大请求上限的POST报文，预期服务拒绝并主动断开连接，防止内存耗尽OOM
    oversized = (
        b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2000000\r\n\r\n"
        + b"x" * (1024 * 1024 + 65536)
    )
    expect_connection_closed(oversized)

    # ===================== 用例6：Keep-Alive串行多次请求稳定性测试 =====================
    # 测试目标：长时间复用长连接，多次收发不会造成解析器状态错乱
    ka = HttpConnection()
    try:
        for path in ["/", "/user/456", "/"]:
            raw_req = f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode()
            status, _, body = ka.request(raw_req)
            require(status == 200, f"keepalive请求 {path} 失败，状态码:{status}")
    finally:
        ka.close()

    # ===================== 用例7：HTTP流水线Pipeline高压测试【重中之重】 =====================
    # 场景说明：一次性向socket缓冲区写入上万条请求，模拟TCP数据包合并粘包
    # 暴露缺陷：解析器处理完单次请求没有重置状态机、缓冲区数据残留、请求响应配对错乱
    pipeline = HttpConnection()
    try:
        request_count = 10_000
        single_req = b"GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n"
        # 批量一次性发送全部请求，不等待任何响应（流水线特征）
        pipeline.socket.sendall(single_req * request_count)
        # 循环依次读取对应数量响应，一一配对校验
        for index in range(request_count):
            status, _, body = pipeline.read_response()
            require(status == 200, f"流水线第{index}条请求失败 status={status}")
            require(body == b"fast", f"流水线第{index}响应内容异常 {body!r}")
    finally:
        pipeline.close()

    # ===================== 用例8：静态资源Range分片、ETag缓存机制（206 Partial Content / 304 Not Modified） =====================
    # 测试静态文件服务能力：字节范围请求、条件缓存请求
    files = HttpConnection()
    try:
        # 请求文件前10个字节分片
        status, headers, body = files.request(
            b"GET /logo HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-9\r\n\r\n"
        )
        require(status == 206, f"Range分片未返回206，状态码:{status}")
        require(len(body) == 10, f"分片数据长度错误 {len(body)}")
        content_range = headers.get("content-range", "")
        if content_range:
            require(content_range.startswith("bytes 0-9/"), f"Content-Range格式异常 {headers}")
        else:
            require(headers.get("accept-ranges") == "bytes", "静态资源未声明Accept-Ranges首部")
        etag = headers.get("etag")
        require(bool(etag), "静态文件缺失ETag缓存标识首部")

        # If-None-Match条件请求，校验缓存304逻辑
        status, headers_304, body = files.request(
            b"GET /logo HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: "
            + etag.encode("latin1")
            + b"\r\n\r\n"
        )
        # 兼容两种框架实现：支持304缓存 或 直接返回完整资源200
        if status == 304:
            require(body == b"", "RFC规定304响应必须没有body")
        else:
            require(status == 200, f"If-None-Match返回非法状态码 {status}")
            require(len(body) > 0, "200完整资源响应body不能为空")

        # 超大稀疏文件远端分片边界测试 /large（1GB稀疏文件）
        status, headers, body = files.request(
            b"GET /large HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Range: bytes=1073741814-1073741823\r\n\r\n"
        )
        if status == 404:
            print("skip /large range check: 服务未注册/large路由")
        else:
            require(status == 206, f"大文件Range状态码异常 {status}")
            require(len(body) == 10, f"大文件分片长度错误 {len(body)}")
            content_range = headers.get("content-range", "")
            if content_range:
                require(
                    content_range == "bytes 1073741814-1073741823/1073741824",
                    f"大文件Content-Range错误 {headers}"
                )
            else:
                require(headers.get("accept-ranges") == "bytes", "/large未支持Range分片")
    finally:
        files.close()

    # ===================== 用例9：Reactor事件循环隔离验证【架构级核心用例】 =====================
    # 验证目标：耗时业务任务不能阻塞主线程事件循环
    # 预期行为：/slow长时间阻塞任务运行期间，/fast请求可以立刻响应
    # 如果fast被阻塞，说明业务代码直接运行在Reactor主线程，架构严重缺陷
    import threading

    fast_ok = threading.Event()

    def request_fast():
        """子线程并发发送/fast请求，成功则标记事件"""
        try:
            conn = HttpConnection()
            status, _, body = conn.request(
                b"GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n"
            )
            require(status == 200, f"/fast 状态异常 {status}")
            require(body == b"fast", f"/fast body异常 {body}")
            conn.close()
            fast_ok.set()
        except Exception:
            pass

    # 发起耗时slow请求，不等待响应，让slow任务持续运行
    slow_sock = socket.create_connection((HOST, PORT), timeout=15)
    slow_sock.settimeout(15)
    slow_sock.sendall(b"GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n")

    # 短暂休眠，保证slow任务已经调度执行
    time.sleep(1)

    # 启动线程并行访问fast接口
    t = threading.Thread(target=request_fast)
    t.start()
    # 最多等待5秒获取结果
    t.join(timeout=5)
    # 核心断言：slow任务运行时fast必须正常响应
    require(fast_ok.is_set(), "/fast 被/slow阻塞！耗时任务占用Reactor主线程")
    slow_sock.close()


def stop_server(process: subprocess.Popen[bytes]) -> None:
    """
    安全终止待测服务进程组
    关键细节：启动进程时使用start_new_session创建独立进程组
    killpg杀死整个进程组，可以回收服务fork/spawn创建的所有子进程、子协程工作进程，杜绝孤儿进程残留
    关闭策略：优雅关闭SIGTERM → 超时强制SIGKILL
    """
    if process.poll() is None:
        try:
            # 发送终止信号，允许服务执行资源释放、优雅退出逻辑
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            # 进程已经退出，无需处理
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            # SIGTERM无法正常退出，强制杀死整个进程组
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
    # 轮询等待端口彻底释放，避免紧接着执行压测脚本端口冲突
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and _has_tcp_listen(PORT):
        time.sleep(0.05)


def main() -> int:
    """
    测试程序主入口，完整生命周期控制
    执行流程：
    1. 解析命令行参数，校验二进制文件存在
    2. 清理旧进程，保证端口空闲
    3. 创建静态资源目录与稀疏测试文件
    4. 拉起待测服务进程，捕获stdout/stderr日志
    5. 等待服务就绪，批量执行所有测试用例
    6. 测试成功/失败统一终止服务、清理临时文件
    返回值：0=全部用例通过；异常抛出非0退出码，CI流水线识别为测试失败
    """
    parser = argparse.ArgumentParser(description="自研HTTP服务黑盒回归测试工具")
    # 必填参数：待测http服务二进制绝对/相对路径
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    server_bin = args.server.resolve()
    if not server_bin.is_file():
        parser.error(f"待测服务可执行文件不存在：{server_bin}")

    # 前置清理旧进程
    reclaim_port_for_server(server_bin)
    assert_port_available()

    # 创建静态资源目录，生成1GB稀疏文件large.bin
    # 使用truncate创建稀疏文件：只占用元数据，不消耗1GB磁盘空间，容器友好
    static_dir = server_bin.parent / "static"
    static_dir.mkdir(parents=True, exist_ok=True)
    large_fixture = static_dir / "large.bin"
    with large_fixture.open("wb") as fixture:
        fixture.truncate(1024 * 1024 * 1024)

    # 临时文件缓存服务运行日志；测试成功自动丢弃，失败打印日志便于排查
    with tempfile.TemporaryFile() as log_buffer:
        proc = subprocess.Popen(
            [str(server_bin)],
            cwd=server_bin.parent,
            stdout=log_buffer,
            stderr=subprocess.STDOUT,  # stderr重定向到stdout，统一保存日志
            start_new_session=True,    # 创建独立进程组，方便killpg整体回收
        )
        try:
            wait_until_ready(proc)
            run_checks()
            print("✅ all black-box HTTP checks passed")
            return 0
        except Exception:
            # 捕获所有异常，读取日志尾部输出，限制字符长度防止CI日志刷屏
            log_buffer.seek(0)
            server_output = log_buffer.read().decode("utf-8", errors="replace")
            if server_output:
                print("========== server runtime output ==========")
                print(server_output[-8000:])
            # 重新抛出异常，shell获得非0退出码
            raise
        finally:
            # 无论测试成功、失败，一定会执行资源回收逻辑
            stop_server(proc)
            reclaim_port_for_server(server_bin)
            # 删除测试生成的临时稀疏文件
            large_fixture.unlink(missing_ok=True)


if __name__ == "__main__":
    # SystemExit包装main返回值，向操作系统传递进程退出码
    raise SystemExit(main())