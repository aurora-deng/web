# gRPC 消息封装教学版

这个目录只拆开 gRPC 在 HTTP/2 `DATA` 负载里的 **5 字节消息信封**：

```text
+--------------------+----------------------+------------------+
| compressed: 1 byte | length: 4 bytes (BE) | message bytes... |
+--------------------+----------------------+------------------+
```

可以把 HTTP/2 DATA 帧看成货车，把 gRPC message 看成快递盒。一个盒子可能被两辆车分段运送，一辆车也可能装多个盒子，所以 `MessageDecoder::feed()` 支持任意分片和一次解析多条消息。

本教学版故意不重复实现 HTTP/2、HPACK、Protobuf、压缩和 trailers；这些由生产路径中的 nghttp2/gRPC/Protobuf 各自负责。`compressed=1` 只保留标志和原始负载，不做实际解压。

```bash
cmake -S server/grpc/learn -B build-grpc-learn
cmake --build build-grpc-learn
./build-grpc-learn/grpc_message_learn
```

建议按 `encodeMessage()` → `MessageDecoder::feed()` → `main.cpp` 的顺序阅读，再到 Phase 8 对照完整的 HTTP/2 headers、DATA、trailers 链路。
