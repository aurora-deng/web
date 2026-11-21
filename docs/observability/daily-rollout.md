# 日常 18090 日志与函数耗时启用记录

2026-11-19 10:17（北京时间）经用户明确批准完成启用。Linux 保留原通信核心、原 `phase11` 数据库和原页面；本次更换业务程序及配套 ONNX Worker，TTS Worker 内容未变。没有执行迁移，没有新增账号、果实或聊天测试消息。

## 已验证的范围

| 项目 | 结果 |
| --- | --- |
| 日常 HTTP / HTTPS | 18090 / 18443；存活、就绪及三类页面均为 200 |
| 程序与功能开关 | gRPC、ONNX、RocketMQ、TTS、PostgreSQL、认证等开关与原日常配置一致；动态库均可解析 |
| 通信核心 | `backend/server/` 152 个文件与原日常源码逐一一致，未修改 |
| 原数据库 | 仍为 `phase11`、迁移版本 24；未执行 DDL |
| 原账号 | 68 个原账号 ID、用户名和密码散列保持不变；既有演示账号真实登录通过 |
| 原页面与密钥 | 运行目录前端文件、TLS 证书/私钥和 Token Hash 密钥摘要均不变 |
| 实际业务读取 | 会话列表、前后历史、树分类/列表/榜单及语音状态读取通过 |
| 调用链 | `494901e7a03820145fc17cef14d80a3b` 为完整 8 段链路，包含独立探针、业务服务和 PostgreSQL 调用 |
| 日志 | 收到主动业务日志、Observer 诊断及独立标记的 Trace 派生记录；文件拒收和丢弃均为 0 |
| 保留期限 | 日志索引滚动七天，后台每分钟检查；自有文本按 UTC 日轮转，保留今日及前六天、每天上限 8 MiB |

程序 SHA-256：`865fdaefa336d7235132f0da287acea664c04425b552641b529efe71be2b8b99`。启用后的 PID 为 `57254`，它只用于本次记录，后续以 `service.py status` 为准。

完整配置 Release 回归为 117 项通过、1 项可选 Redis 契约跳过。Debug/UBSan 是日志专项验证，不能据此声称整个工程的 Debug、TSan 或生产验收已完成。隔离混合负载、浏览器结果和剩余限制见[验收记录](validation.md#2026-11-19-日志查询编辑器与函数耗时)。

## 现在查看什么

- [日常函数耗时](http://127.0.0.1:18791/?view=profiles)：核对当前目标为 `127.0.0.1:18090`，能力状态为已收到 `nebula_function_seconds`。
- [日常日志](http://127.0.0.1:18791/?view=logs)：筛选 `nebula-webserver`，再按级别、关键词、Trace 或请求编号下钻。
- [本次真实查询链路](http://127.0.0.1:18791/?view=traces&trace=494901e7a03820145fc17cef14d80a3b&from=1795054830&to=1795054870)：从探针到会话业务和数据库，时间窗已固定。
- [本次只读函数窗口](http://127.0.0.1:18791/?view=profiles&from=1795054830&to=1795054870)：没有向原库写测试消息，因此这次窗口里的发送函数允许显示无调用。

在聊天室实际切换会话、翻历史或发送消息后，新样本会进入对应面板。函数耗时是墙钟时间，包含等待；桶估计的 P99 不是某一次请求的准确耗时，也不等同客户端 ACK 往返。单次执行看调用链，线程 CPU、分配和内核观察仍需对应剖析报告。

Windows 访问依赖 SSH 转发。现有转发工作时直接打开页面；关闭后可重新运行下面命令并保持终端开启：

```powershell
& "$env:WINDIR\System32\OpenSSH\ssh.exe" -o ExitOnForwardFailure=yes -N `
  -L 127.0.0.1:18791:127.0.0.1:18791 `
  -L 127.0.0.1:18793:127.0.0.1:18793 pikachu@192.168.239.135
```

## 正常启动与监控

```bash
cd /home/pikachu/function3-src
python3 scripts/function3/runtime/service.py status
# 仅在服务停止后启动；默认读取已经授权的 observability.enabled 标记。
python3 scripts/function3/runtime/service.py start
```

业务重启会改变 PID，监控应按[启动说明](setup.md#日常聊天室与苦情树)重新读取 PID。Observer 的启动参数已加入 `--local-otlp` 和业务 `--log-input-directory`；只配置 `--local-target` 不能自动收到链路和日志。停止业务保留 PostgreSQL、Redis 和原文件。

临时不采集可使用 `start --no-observability`，下一次普通启动仍按授权标记开启。当前业务日志级别为 WARN，HTTP 每个工作线程首次及之后每 20 次采样，WS 每 100 次采样；直方图统计不随 Trace 抽样丢失。

## 备份与回退

本次备份位于：

```text
/home/pikachu/phase11-data/function3/runtime/backups/observability-20261119T021733Z-d75b1d
```

其中包含原程序、配套 Worker、启动脚本、原运行环境的私有快照及 `phase11.dump`。数据库备份已用 `pg_restore --list` 校验可读取。目录权限为 0700，环境快照包含密钥配置，不要上传或粘贴到公开文档。程序/数据库备份与七天遥测保留分开管理，本次不自动删除备份。

若仅需暂停采样，使用上面的 `--no-observability`，不必退回旧程序。需要完整回退时：

1. 用日常 `service.py stop` 停止它拥有的进程；脚本检查路径与启动世代，等待优雅退出，不强杀其他服务。
2. 从备份 `build-release/` 原位恢复 `webserver`、`webserver-onnx-worker` 和 `backend/application/modules/function3-tts-worker`，保留执行权限；恢复备份的 `service.py`。
3. 恢复备份里的授权标记；备份没有该标记时移除本轮 `runtime/observability.enabled`。用私有 `runtime-private.json` 中的原 argv、env、cwd 启动，更新 PID 与 ticks。不要另外生成 Token 密钥或证书。
4. 以新 PID 重启 18791 的采集进程，按备份 `observer-private.json` 恢复旧参数。确认 HTTP/HTTPS 就绪、原账号登录及原库身份。

本轮没有修改数据库结构或正文，程序回退通常不需要恢复数据库。不要为回退程序直接覆盖正在使用的原库。切换脚本在启动、身份或完整性检查失败时自动按原私有环境回退；本次未触发回退。
