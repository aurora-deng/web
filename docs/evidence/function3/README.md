# Function3 验证证据

- `release-final-ctest.txt`、`debug-ctest.txt`、`asan-ctest.txt`：最终完整回归，各107项。
- `debug-summary.json`、`asan-summary.json`：编译参数、耗时与日志SHA。
- `benchmarks/`：最终三轮基线/当前的wrk与混合业务原始数据；`summary.json`保留门槛未通过。
- `benchmarks-before-*/`：此前测量与失败记录，未删除不利结果。
- `business-final.txt`：真实HTTP/PG/MQ/TTS、HTTP/2业务与故障恢复。
- `mq-final.txt`：真实FIFO/NORMAL/NACK重投。
- `ai-mixed.json`：既有Phi3的后台启用、两次真实生成与普通聊天混合；不是质量评估。
- `capacity-final.json`：十万动态的分区抽样与资源记录。
- `wal-diagnostic.json`、`wal-diagnostic.txt`：计时对照及原始输出；结果从完整日志恢复，收尾探测竞争已修复，设置另行核验恢复。
- `retained-binaries.json`：清理生成对象前后保留的相同二进制摘要。
- `MANIFEST.json`：归档白名单文件SHA和对应源码指纹。

计时诊断不能替代三轮性能门槛。完整结论见 [Phase34](../../architecture/business/phase34.md)。归档不包含真实用户数据、登录Cookie、密码、私钥、模型和上传文件。

`windows-portable.json` 为本地Windows补充检查，独立于Linux归档清单。Linux源码指纹按大小写敏感的POSIX相对路径排序计算，Windows复核时也须显式按 `as_posix()` 排序。401份相关源码的指纹已在两端核对一致。
