# TLS/ALPN 内存双端教学版

这里有两件小实验。`record_main.cpp` + `Record.h` **手写 TLS 外层 5 字节记录信封的增量拆包**，故意把合成字节逐个喂入，观察 TCP 分片；它不解析内部握手消息，也不解密。`main.cpp` **真的使用 OpenSSL 完成 TLS 1.3 握手和加解密**，但没有监听端口、Reactor、业务路由，也没有手写密码算法。`BIO_new_bio_pair()` 把客户端和服务端接在内存里，便于单步观察双方怎样反复调用 `SSL_do_handshake()`，如何处理 `WANT_READ/WANT_WRITE`，以及如何用 ALPN 选择 `h2` 或 `http/1.1`。生产服务器则用非阻塞 socket、epoll 和 `TlsSession` 推进同样的状态。

先为**本地练习**生成自签名证书，再独立构建运行：

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout /tmp/tls-learn-key.pem -out /tmp/tls-learn-cert.pem \
  -subj '/CN=localhost'
cmake -S server/tls/learn -B build-tls-learn
cmake --build build-tls-learn
./build-tls-learn/tls_record_learn
./build-tls-learn/tls_learn /tmp/tls-learn-cert.pem /tmp/tls-learn-key.pem
./build-tls-learn/tls_learn /tmp/tls-learn-cert.pem /tmp/tls-learn-key.pem --h1
```

1. `RecordParser::feed()` 等齐 5 字节头，再按大端长度等待完整 payload；这和 HTTP/2 的增量帧解析思路相似，但 TLS 加密记录的内部内容不能靠外层 type 看出来。参考 [RFC 8446 第 5 节](https://www.rfc-editor.org/rfc/rfc8446.html#section-5)。
2. `ClientHello` 提供 ALPN 列表，格式为每个协议前有 1 字节长度；`selectAlpn()` 优先选 `h2`，只提供 HTTP/1.1 时选它。
3. 两端交替调用 `SSL_do_handshake()`，OpenSSL 处理真实 TLS 记录、证书与密钥协商；`WANT_*` 表示需要再推进，不是握手失败。
4. `SSL_get0_alpn_selected()` 取最终协议。随后客户端 `SSL_write_ex()` 送应用字节，服务端 `SSL_read_ex()` 收到解密后的原文。
5. 生产版映射：`TlsContext.cpp` 配证书与 ALPN；`TlsSession.cpp` 等 epoll 可读/可写并按协议交接；`TlsTransport.cpp` 包装读写；`TransportWriter.cpp` 把响应先经 TLS 加密再送 socket。

演示客户端为接受临时自签名证书而关闭验证；**真实客户端必须验证证书链和主机名**。本演示不解析或伪造 TLS 记录，更不能作为通用 TLS 实现。
