# Phase49 公开验证证据

功能回归通过，性能门槛未全部通过。基线 `42b3f8f`；2026-10-30至31。

- `summary.json`：分类结论、验证顺序、范围和限制。
- `structure.json`：main 964→14行、37→0个入口unique_ptr；119项方法/路径。
- `source.json/source.log`：最终源码门禁、33项前端与14项工程测试。
- `linux/`：103份筛选导出文件，含Linux三种矩阵、12项启动失败、真实业务、Worker、可选驱动关闭与最终运行版本。
- `linux/benchmarks/`：第一次中断的原记录，未作为三轮验收。
- `linux/benchmarks-resume-after-interruption/`：完整三轮，/fast中位-3.89%，第三轮ACK +78.47%，混合吞吐中位-16.93%，业务错误0，整体验收失败。
- `linux/benchmark-analysis.json`：聚合耗时对照；不是逐消息因果追踪。
- `windows-*.json`：两种客户端构建及真实TLS/WS/SSE/私人模型、迁移、Agent、语音回退；资源只计原生进程。
- `ui/`和浏览器日志：真实前端加协议替身；最终聊天补验覆盖非空知识资料。原超时、PG离线和测试端口错误日志保留。
- `runtime-cross-platform.json`：最终版本、资源清单、迁移024、Windows/Linux源码指纹一致。

测量二进制与最终状态页构建摘要分开保存于`linux/binary-fingerprints.json`，不可混用。原始文件使用本目录.gitattributes保留字节，`delivery-sha256.json`用于复核。

未包含数据库、模型、用户上传、私钥、云Key、私人资料或可执行文件；未推送远程、未部署日常服务，隔离服务器/Worker已停止。已有Phase41/42未跟踪证据未修改。

讲义：[本轮改动](../../../docs/architecture/business/phase49.md)、[数据库分层](../../../docs/database/domain-layering.md)、[接口教学](../../../docs/architecture/business/phase49-teaching.md)。
