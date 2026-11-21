# 从主机、进程和函数定位长尾

## 页面入口

- 「运行总览 → 主机 CPU / 内存使用率 → 查资源占比」保留所选时间。进入「主机与进程」可以展开逻辑 CPU、按服务选择进程，查看 RSS、PSS、用户态/内核态、调度、磁盘 I/O 和线程。
- 「函数与内核剖析」显示 NEBULA 四个业务函数的调用速率、墙钟平均、P95 和 P99。「看调用链」打开对应函数的单次 Trace，包括父子 span、时间、状态与属性。实验报告还展示绑核前后、syscall、指令和堆分配调用栈。查看[函数下钻步骤](explore-and-connect.md#从函数页下钻)。
- 「数据源」可以配置 Pyroscope 和 Jaeger v3。Pyroscope 在剖析页加载服务器已有的 Profile Type，按服务标签选择 CPU 或堆样本。Jaeger 在「分布式排障」按服务名或 Trace ID 查询，沿用 span 瀑布图和 Loki 的日志关联。同一指标体系下的远端机器可通过 Prometheus 接入 Process Exporter、Node Exporter 和 cAdvisor，模板可选实例、命名空间与 Pod。
- 一次隔离服务运行时，采集器按启动配置读取该 Linux 机器的 NEBULA、PostgreSQL 及可见子进程。其他服务可用 --resource-target NAME=PID 注册。注册后的起始 tick 有变化时跳过重用的 PID。跨机器的进程历史和容器维度由 Prometheus 采集器提供。

本轮 Linux 隔离入口为 18791。Windows 本机若没启动转发，运行以下命令，再访问 [127.0.0.1:18791](http://127.0.0.1:18791/?view=resources)。

```powershell
C:\Windows\System32\OpenSSH\ssh.exe -N -L 127.0.0.1:18791:127.0.0.1:18791 pikachu@192.168.239.135
```

## 指标口径

| 指标 | 读法 |
|---|---|
| 主机 CPU 构成 | 由 /proc/stat 相邻采样增量计算。用户态、内核态、中断、I/O 等待、steal、空闲分开显示。 |
| 进程 CPU | 100% 代表占满一颗逻辑 CPU，可以超过 100%。整机容量占比用整机所有 CPU tick 作分母；已忙 CPU 占比扣除空闲、I/O 等待和虚拟化抢占。 |
| RSS 与 PSS | RSS 是驻留页，可能重复计入共享页。PSS 按共享比例分摊。线程共用进程地址空间，页面不分摊每线程 RSS。 |
| 内核指标 | /proc 的调度、缺页、交换、重传、丢弃及监听队列计数增量。就绪等待需要 schedstats，PSI 需要主机暴露接口。缺测显示空值。 |
| 函数平均和 P99 | 只对四个已埋点的 ChatService 函数计时，包含调用与等待。P99 来自直方图，低样本窗口用于定位趋势。 |
| 单次函数墙钟 | 显示追踪样本中的父子调用。已知子调用之外的时间仍可能包含阻塞和未采样工作。 |
| Callgrind / strace | Callgrind 给自身指令数，并统计由函数发起的系统调用墙钟、内核 CPU 纳秒。strace 提供多线程系统调用汇总，futex 或 epoll 等等待会在各线程累加。 |
| DHAT | 量化累计分配、全局堆峰值时存活量和退出前存活量，并展示分配调用栈。退出前存活量不能单独用于判定泄漏。 |
| Pyroscope | 火焰条长度取决于后端采样类型与单位。选择 CPU 纳秒、分配字节或存活堆时要读取相应 Profile Type。 |

先从「服务目标与采集质量」确认影响范围，再按同窗检查资源利用率、饱和度与错误。CPU 使用率可对照线程就绪等待，磁盘吞吐可对照 I/O 等待和错误，连接池使用可对照任务积压。缺少 PSI 或调度统计的机器用现有 Linux 计数与受控实验验证。[USE 方法](https://www.brendangregg.com/usemethod.html)

## 运行隔离实验

在 Linux 根目录执行，填写实际已有的工具链和二进制位置：

```bash
python3 -m tools.observability.resource_lab \
  --binary "$BINARY" --postgres-prefix "$POSTGRES_ROOT" \
  --output "$RUN_ROOT/resources-$(date +%s)" --cpus 2-3 --rounds 3 --requests 300
```

未绑核与绑核轮次交替，业务服务在隔离数据库中启动。只限制服务进程的 CPU 亲和性；数据库和客户端仍沿用原调度。剖析轮次分别用 strace、Callgrind、DHAT 加载，保持少量请求。P99 对照只比较无 profiler 的轮次。报告保留二进制摘要、各轮时间和 CPU 掩码，原始输出留在实验目录。

```bash
python3 -m tools.observability.observer.server import-resource \
  --db "$RUN_ROOT/events.sqlite" "$REPORT/report.json"
```

定位时在报告点开某一轮，在「ACK 与同区间资源」对照 ACK 样本数与 P99、进程用户态与内核态、RSS、主机切换、主缺页和重传。第一个资源区间包含账号准备，后续区间更适合观察持续负载。用函数墙钟筛选代码段，再用 CPU profile 或堆分配栈验证假设。SSH 时区或显示时区可以不同，实验报告保存 Unix 时间。采集不更改 sysctl、内核或正式服务。

也可用 `scripts/function3/validation/function32_lab.py profile --workers 16 --operations 4000 --binary "$BINARY" --postgres-prefix "$POSTGRES_ROOT" --output "$ACK_REPORT"` 跑独立的混合负载分段实验。导入时增加 `--ack-profile "$ACK_REPORT/report.json"`。服务校验二进制摘要一致，页面在「混合负载 ACK 逐条分段」显示各阶段 P99 和最慢请求的逐条时长，过滤消息 ID。

逐条表格的「未细分」用同一消息的客户端 RTT 减去服务端排队和任务开始到 ACK 就绪。剩余区间可能包括收发、事件循环调度及客户端处理。出现缺值或负差时显示空值。取连接和 COMMIT 已包含在任务处理时间内，不能再扣一次。几条 P99 来自各自的分布，查看慢样本时用逐条数据。

## 关联 COMMIT 和数据库等待

在 Linux 使用本次代码编译的服务端和已安装的 PostgreSQL 18 工具链。先看自然负载，需要验证采样器时另跑一次带刻意延迟的控制实验。

```bash
python3 scripts/function3/validation/function32_lab.py commit \
  --binary "$BINARY" --postgres-prefix "$POSTGRES_ROOT" \
  --output "$RUN_ROOT/commit-observation" --workers 16 --operations 4000

python3 scripts/function3/validation/function32_lab.py commit \
  --binary "$BINARY" --postgres-prefix "$POSTGRES_ROOT" \
  --output "$RUN_ROOT/commit-control" --workers 1 --operations 30 --commit-delay-ms 120
```

- `report.json` 的 `instrumented.commitObservation` 包含最长的 20 次提交、自然负载的慢提交数、命中覆盖率和最长采样间隙。每条客户端 ACK 先按持久化消息 ID 关联服务端分段，再用 PostgreSQL 连接进程号和同一 Linux 单调时钟的提交区间匹配等待。查询的起止时间必须完整落在对应提交内。
- `commit-samples.json` 保留原始等待、WAL 和磁盘采样。等待按约 10 ms 采样。短于采样间隔的等待可能漏掉。报告中 `active` 表示采到该进程正在执行但当时未报告等待事件。
- `diskDelta` 仅在提交内有至少两点可计算时出现。逻辑卷、分区和设备各自读数，跨层不能相加。WAL 累计差值覆盖近似工作窗口和统计发布时间，适合看整轮变化。可以用 `IO:WalSync` 样本确认该进程正在等 WAL 同步，用同窗磁盘读数辅助定位。[PostgreSQL 等待与 I/O 统计](https://www.postgresql.org/docs/18/monitoring-stats.html) · [Linux 磁盘计数器](https://www.kernel.org/doc/html/latest/admin-guide/iostats.html)
- 控制实验用临时数据库的延迟约束触发器，在第一条实测消息提交时等待 `pg_sleep`。应采到同一进程的 `Timeout:PgSleep`，且对应 `commitUs` 至少为命令中指定的时长。控制轮的 P99 留作采样验证，性能比较使用无延迟轮。WAL 计时只在本轮临时 PostgreSQL 进程启动时开启。

### 在 Observer 复盘

将隔离实验的 `report.json` 归档到当前 Observer 实例使用的 SQLite 文件。`OBSERVER_DB` 指向启动 Observer 时 `--db` 传入的路径。

```bash
python3 -m tools.observability.observer.server import-commit \
  --db "$OBSERVER_DB" --id commit-natural-01 --label '自然负载 · 第一轮' \
  "$RUN_ROOT/commit-observation/report.json"

python3 -m tools.observability.observer.server import-commit \
  --db "$OBSERVER_DB" --id commit-control-01 --label '120 ms 延迟对照' \
  "$RUN_ROOT/commit-control/report.json"
```

打开「函数与内核剖析 → 提交诊断」并选报告。「最慢提交」按提交墙钟耗时排序，点击一行查看对应 ACK、排队、命中等待事件及磁盘采样区间。地址栏链接可重新打开当前报告及所选样本。给每轮使用独立的报告 ID，避免新的归档覆盖已分享的报告。「整轮 WAL、磁盘与采样设置」显示整场实验的增量与采样配置。

旧实验使用单调时钟记录区间，页面显示归档时间和相对偏移。没有匹配样本或缺少磁盘差值时，页面单独提示。导入只存展示需要的测量值，后台 PID 与原始采样文件留在隔离实验目录。两类剖析报告共用 24 份归档上限，请保留原始实验文件。

[本轮实测](validation.md) · [Linux proc](https://docs.kernel.org/filesystems/proc.html) · [PSI](https://docs.kernel.org/accounting/psi.html) · [Callgrind](https://valgrind.org/docs/manual/cl-manual.html) · [DHAT](https://valgrind.org/docs/manual/dh-manual.html) · [Pyroscope 查询 API](https://grafana.com/docs/pyroscope/latest/reference-server-api/) · [Jaeger v3 API](https://github.com/jaegertracing/jaeger-idl/blob/main/swagger/api_v3/query_service.openapi.yaml) · [Process Exporter](https://github.com/ncabatoff/process-exporter)
