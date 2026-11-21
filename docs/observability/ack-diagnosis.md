# 用真实慢 ACK 学习指标、Trace 与故障恢复

目标：用一次数据库行锁解释聊天 ACK 为什么变慢。采用已有通信核心与真实 PostgreSQL；只向一次性数据库写入合成测试账号和消息。日志、事件和 Trace 不记录聊天正文或私有资料。

## 1. 先认清三张图

1. **指标像体温表**：函数直方图和阶段 P99 反映一个时间窗的分布。有调用才有延迟；零值、无调用和采集失败分别显示。
2. **Trace 像快递过站记录**：同一个请求依次经过 WS 业务任务、聊天服务、数据库命令、COMMIT 和 ACK 就绪。墙钟时间包含执行与等待，不能当成精确 CPU 或内存。
3. **实验像维修工单**：记录哪时加了故障、恢复前后数据、版本和重复请求的数据库结果。Trace 找慢段，实验复核结果。

## 2. 启动真实指标档

先按[原生交付](../ops/validation/native-delivery.md)构建，或显式使用已受测的业务二进制。下面 `RUN` 必须是新的隔离目录；依赖复用已批准安装。

```bash
RUN=/home/pikachu/phase11-data/function3/observer-review-example
python3 -m tools.observability.lab --profile metrics \
  --dependencies /home/pikachu/phase11-deps/observability \
  --postgres-prefix /home/pikachu/phase11-deps/postgresql/18.6 \
  --binary build/repro-native/webserver --work-dir "$RUN" \
  --observer-port 18798 --prometheus-port 19091 \
  --node-port 19101 --probe-port 18796 \
  --duration 14400 --exercise --http-trace-every 1 --ws-trace-every 1
```

该档启动 Observer、Prometheus、Node Exporter、Probe，以及临时 PG 和 NEBULA。只绑定本机；不启动尚未安装的 Collector/Tempo。事件与 Trace 当前由本机索引保存，不能称为已经联调真实 Tempo。

Prometheus 使用目标发现文件与私有 ops 凭据。隔离目标端口重建时更新发现文件；启动验证匿名 401、普通身份 403、ops 200。正式共享数据仍留在原业务库，以上实验不会写入它。

Windows 访问：

```powershell
& "$env:WINDIR\System32\OpenSSH\ssh.exe" -N -o ExitOnForwardFailure=yes `
  -L 127.0.0.1:18798:127.0.0.1:18798 pikachu@192.168.239.135
```

打开 `http://127.0.0.1:18798`。端口已占用时先核对已有实例，不随意终止其他进程。

## 3. 制造并解释慢请求

另一个 Linux 终端在同一源码目录运行：

```bash
python3 -m tools.observability.ack_diagnosis \
  --run-dir "$RUN" \
  --postgres-prefix /home/pikachu/phase11-deps/postgresql/18.6 \
  --output "$RUN/exports/ack-diagnosis.json"
```

脚本核对实验 owner、一次性库名和临时 Unix 套接字，拒绝原库和日常端口。真实指标档先等待当前目标的第一份计数器基线，再发送测试消息；首次抓取之前已经发生的请求不能凭空形成速率样本。随后自动执行：

1. 两名测试用户成为好友，建立私聊，发送二十条基线消息。
2. 独立 PG 会话锁住这条测试会话行约 1.5 秒；在确认锁持有后发送一条消息。
3. 恢复后发送二十条消息，重新提交慢消息的同一幂等编号。
4. 检查完整服务端 Trace 中的数据库等待、ACK 已确认结果和唯一消息行数，并保存前/故障/后三组实验记录。

大盘操作：

- 「业务性能」对齐故障时间，观察聊天阶段、队列和连接池。P99 是桶估计，不能与单条 RTT 做相等比较。
- 「链路追踪」筛选 `WS chat.send`、最短耗时 500 ms，打开报告中的 Trace ID。比较 `PostgreSQL command`、COMMIT 和 ACK 就绪，确认锁等待发生在实际命令中。
- 「日志工作台」按同一 Trace ID 查关联记录，正常无报错也允许没有 ERROR；不能靠制造假错误填满面板。
- 「研发实验」核对同窗故障标记、恢复记录与 `matchingRows=1`。本例样本少且包含人为故障，不作吞吐或性能提升结论。

独立探针的跨进程链路可另行检查：

```bash
python3 -m tools.observability.probe trace --run-dir "$RUN"
python3 -m tools.observability.verify_metrics --run-dir "$RUN" \
  --exercise-outage --output "$RUN/exports/metrics-validation.json"
```

同一 traceId 下应出现探针、NEBULA HTTP、业务函数与 PostgreSQL 子调用。部分链路与后端不可用有不同状态。

`verify_metrics` 只对已核对归属的实验 Prometheus 暂停/恢复进程，检查指标不可用为 503、Observer 仍存活、已存链路可读和恢复后曲线可查；始终在 `finally` 恢复进程，不触碰日常服务。

## 4. 七天保留和停止

Prometheus 保留七天、块数据预算 384 MB；指标档全部实验数据达到 512 MiB 或磁盘空间不足会停止。Go 内存参数是软预算，不能当硬内存隔离。Observer 的事件、Trace、日志索引与自有轮转文件按七天清理。

实验目录在创建时标记临时遥测。启动新实验会检查同级已过期、已停止且有标记的旧实验；仅清理已列明的遥测文件/目录。也可先查看计划，再执行：

```bash
python3 -m tools.observability.retention --root /home/pikachu/phase11-data/function3
python3 -m tools.observability.retention --root /home/pikachu/phase11-data/function3 --apply
```

未标记目录、活动实验、原库、用户文件与 `exports` 不清理。没有安装全局定时任务；未启动监控时需要主动执行该清理命令。

前台实验按 `Ctrl+C` 停止；后台实验先核对 `RUN/lab.pid` 的命令确实是这个目录的 `tools.observability.lab`，再对该 owner 发 `SIGTERM`。owner 会关闭自己启动的组件和临时库。不要按程序名批量杀死所有 PostgreSQL 或 webserver。

## 5. 自检与助教提示词

自检题：行锁为何可能让函数墙钟变大而 CPU 变化很小？数据库等待与 COMMIT 延迟为什么不能混称为“数据库慢”？为什么重复收到 ACK 也不能简单插入第二行？为何本例一个故障样本的 P99 不能代表容量？

可复制的教学提示词：

> 请以当前源码和 `ack-diagnosis.json` 教我排查一次慢 ACK。先核对版本、时间窗、采样和数据源，再逐步解释任务排队、连接池、数据库命令、COMMIT 与 ACK。用简单类比、编号和必要的 C++ 示例，每一步等我说明理解程度后再展开。区分墙钟、CPU、内存与客户端 RTT；保留缺证据和历史失败项。只对当前隔离实验操作，不改通信核心、原库或日常服务；不收集聊天正文和私人资料。最后让我根据恢复后的 Trace、日志和唯一行数复述结论。
