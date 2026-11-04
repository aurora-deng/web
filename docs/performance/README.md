# 性能文档

- [可复现实验方法](BENCHMARK.md)：相同配置、多轮、吞吐/延迟/错误率。
- [业务性能与容量阶段](../architecture/business/runtime/README.md)：Phase15、16、26、28。
- [通信热路径](../architecture/communication/performance/README.md)：Phase10。
- [最新Phase49结果](../architecture/business/engineering/phase49.md#6-性能结果与下一步定位)：/fast中位-3.89%，第三轮ACK P99 +78.47%，严格门槛未通过。
- [下一轮定位建议](../roadmap/next-steps.md)：同消息关联排队、SQL、COMMIT、ACK与客户端往返，不以聚合平均值伪造完整链路。
- [原始证据入口](../evidence/README.md)：保留完整三轮及中断/失败记录。

网络空接口QPS不能代替聊天持久化ACK表现；目录整理也不改变已有性能结论。
