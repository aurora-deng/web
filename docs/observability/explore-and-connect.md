# 用真实业务跑通 Explore、调用路线与监控接入

Windows 通过 SSH 打开 [18791 大盘](http://127.0.0.1:18791/)。以下命令在 Linux 虚拟机里执行。大盘和业务程序使用隔离实验目录。

```bash
LAB=$(cat /home/pikachu/phase11-data/function3/validation/observer-resources-current)
cd "$LAB/app"
```

## Explore 查 P99 和延迟桶

先执行一条真实会话请求，让已登录的独立探针带 `traceparent` 调用 C++ 服务。

```bash
python3 -m tools.observability.probe trace --run-dir "$LAB/run"
```

在「指标探索 → Explore · PromQL 与直方图」选择「NEBULA 本机延迟桶」。等待一次五秒采样，运行：

```promql
histogram_quantile(0.99, sum by (le, route) (rate(nebula_http_handler_seconds_bucket[5m])))
```

时间序列的结果单位是秒，柱条将上界换算为毫秒。经典直方图桶为累计计数，页面用相邻桶之差展示各区间的独立请求数。`+Inf` 是落在最大有限上界之外的样本。每组桶显示实际覆盖时间和观测数，低样本窗口的 P99 是近似值。部署前的历史指标没有原始桶。轻量档支持三类命名直方图、经典桶、五分钟窗口、按该族标签分组与等值过滤。选择外部 Prometheus 执行完整 PromQL。

顶栏变量为 **BC → APP → INSTANCE**。当前 Linux 隔离环境通常显示 `lab → nebula → 127.0.0.1:实际端口`。选择上一级会清空下一级选择。自定义 Prometheus 面板可按真实标签查询实例，并在 PromQL 中使用 `bc=~"${bc}"`、`app=~"${app}"`、`instance=~"${instance}"`。采集端需要实际附有这些标签。

## 看一条调用到底经过哪里

探针会打印一个 `path`。把它接在 Windows 上的 `http://127.0.0.1:18791` 后面，就能看到探针、HTTP 业务处理、`ChatService::conversations`、`PostgresConversationRepository::listConversationSummaries` 和其 PostgreSQL 客户端调用。鉴权可能有并列 SQL span。点击时间条，面板显示该 span 的父级和全路径，能跳到同一时段指标、主机或函数剖析。

瀑布图反映实际采集到的父子关系。数据库 span 是 libpq 调用的墙钟时间。有缺父节点时页面显示部分链路。扣去已知子调用后的剩余墙钟可能包含阻塞或未埋点部分。面板支持服务、操作、耗时和错误筛选；列表可按耗时排序及按函数名检索。

### 从函数页下钻

1. 打开「函数与内核剖析」，在顶栏选择时间范围，再点函数行的「看调用链」。列表按准确的函数名和 NEBULA 服务查找采到的调用，每次最多显示 12 条。
2. 选一条调用，瀑布图会选中对应的业务函数。点父调用或时间条，行下会展开开始时间、墙钟、调用类型、状态和已采集属性。Trace ID 可复制，导出的 JSON 保留起止时间和父子关系。
3. 点「该时段主机与进程」对照本机资源。点「按 Trace 关联日志」会带入选中 span 的时段和 Trace ID。连接 Loki 后还需选择日志流，应用日志需实际包含该 ID。远端 Trace 的主机指标需要匹配对应数据源标签。

打开会话列表、读取新消息、向前翻历史分别产生对应函数的 HTTP 调用。实验进程启动时加 `--ws-trace-every 100`，WebSocket 业务线程会采集首条发送，此后每 100 条采一条。现有服务使用 `WEB_OBSERVABILITY_WS_TRACE_EVERY=100` 加上已启用的遥测导出配置，默认不采 WS 发送。采样从业务处理开始，可看到写入期间的 libpq 调用和 ACK 就绪前的工作。要看执行器排队和客户端 ACK 往返，请继续对照[混合负载分段](resources.md)。

趋势是时间窗口内的直方图聚合。调用链只使用该次调用实际记录的 span。旧窗口可能有 P99、没有对应的 Trace。内置 Trace 最多保留七天，达到 24,000 个 span 时会优先淘汰旧记录。自行采集的 Python 函数可从「查找其他已采样的函数」进入链路页，按函数名搜索。

### 运行自己的 Python 函数

```bash
python3 -m tools.observability.function_trace --run-dir "$LAB/run" demo
python3 -m tools.observability.function_trace --run-dir "$LAB/run" \
  call /home/pikachu/my_job.py main --args-json '[10000]'
```

`main` 要是该文件里定义的同步函数。重复增加 `--watch compute` 可以只记录入口与指定的函数，每次最多记录 128 个函数调用。工具用高分辨率单调时钟测量调用时间，另采线程 CPU。线程计时的粒度可能让短调用出现超出墙钟的读数，此时 CPU 留空，仍可上传和回读 Trace。子调用自动跟踪未标注异常结果，入口执行失败时根 span 标记错误。输出不包含参数、返回值和文件正文。已有 C++ 函数用业务探针或项目 `telemetry::Span` 埋点，原生函数的 CPU 和堆调用栈入口见[资源剖析手册](resources.md)。

## 验证扩展生产者接通

```bash
for kind in dag agent device; do
  python3 -m tools.observability.scenarios "$kind" \
    --observer http://127.0.0.1:18791 \
    --credentials "$LAB/run/producers.json" --events-only --verify
done
```

`first.inserted` 表示持久化，`repeat.duplicates` 验证去重，`readBack` 核对同一来源、同一批事件 ID 确实可读。将输出的 `path` 拼到 18791 入口查看。也可在「扩展遥测」选类别，输入来源名后点「核验该来源」。这些样例保留模拟标记。真实 DAG、Agent、设备生产者用独立来源和相应领域的 Bearer 凭据，遵守[事件契约](architecture.md#事件写入与查询)。同一个事件 ID 重发需要保持内容一致。Linux 本地受信配置存于实验目录的 `producers.json`，启动载入，重启保留已有密钥。跨机器 OTLP 推荐经过受信 Collector。

## 看两条规则怎样变化

| 规则 | 数据和状态 |
|---|---|
| 过去五分钟业务 HTTP 5xx > 5% | 取 `nebula_business_http_responses_total` 中 5xx 增量除以全部已观测业务响应增量。原先 `webserver_http_requests_total` 单独保留为流量指标。五分钟窗口不足、分母为零或计数重置时显示暂无数据。 |
| 数据库历史最大耗时 > 2 秒，持续五分钟 | 指标 `webserver_business_database_microseconds_max`。从采到超过 2,000,000 微秒开始 Pending，连续五分钟后触发。该值记录进程启动后的最高点，一次峰值在进程寿命内可能一直保留。用 P99、样本量和真实链路进一步判断。 |

入口为「告警规则」。点「查看相关时段」可跳到实际时间序列。轻量档由本地评估，完整档生成 Prometheus 规则。

「新建告警规则」可选内置指标、Prometheus 表达式、已采集的文本指标或 HTTP 探测。填写统计方式、比较符、阈值、窗口和等待时间。「试算当前数据」只用真实读数预览一次，保存后由 Observer 后台评估。持续条件按每个序列的新样本计时。漏采或更改规则会重置等待计时。卡片显示当前读数、样本时间、受影响实例、用户影响与处理说明。规则可暂停，最近状态变化可回看。通知目标、定时静默和告警分组待接入。

「函数与内核剖析」首屏提供业务动作、C++ 函数名、调用速率、平均、P95、P99。可搜索、按读数排序，选择「看趋势」对照同一时段的延迟分布。P95 可从已有的经典桶回放，采样保留期内无需重跑旧实验。

## 从告警判断影响

点击业务 5xx 规则旁的「查看请求影响」，在同一时段查受影响请求数、证据覆盖及预算消耗。

- 先核对抓取与保存状态。监控中断时，优先确认采集和查询链路。
- 业务 HTTP 目标用总量和 5xx 数判断影响。点击「查找同窗 HTTP 5xx Trace」，链路列表通过实际响应状态筛选。
- 会话与聊天目标用延迟桶内的达标比例。点「查找同窗慢 Trace」从入口墙钟找候选，再核对内部慢点。调用时间和请求计数的区别见[信号字典](architecture.md#信号字典)。
- 快速判定要求 1 分钟与 5 分钟均达到 3.6×，缓慢判定要求 5 分钟与 30 分钟均达到 1.8×。两对窗口由 6 小时教学预算分别按长窗口消耗 5% 和 15% 折算。缺少覆盖或样本时显示证据不足。

## 导入自己的模板和本地 Git 配置

- 单次导入。进入「自定义仪表盘 → 导入 JSON → 选择文件 → 预览导入」。将源工作区的指标引用绑定到本机已有数据源，再点确认。Observer JSON 导出包含数据源类型等引用信息。Grafana 普通 Prometheus 面板支持常用正则变量，预览会列出跳过的面板和查询数量。
- 文件配置。示例 [nebula-lab.json](../../config/observability/dashboards/nebula-lab.json) 包含 QPS、5xx 比例、函数 P99 和数据库最大耗时。版本控制保存 JSON，Observer 由 `--dashboards-dir` 在启动时应用，运行中也可显式同步：

```bash
python3 -m tools.observability.observer.server provision --check \
  --db "$LAB/run/events.sqlite" --directory config/observability/dashboards
python3 -m tools.observability.observer.server provision \
  --db "$LAB/run/events.sqlite" --directory config/observability/dashboards
```

相同文件再次应用会返回 `unchanged`，改模板时版本递增。页面以「由文件管理」区分；导出后可导入一份独立可编辑副本。Git 本地文件和页面之间是显式单向同步。

[Grafana 追踪视图](https://grafana.com/docs/grafana/latest/visualizations/explore/trace-integration/) · [Prometheus 经典直方图](https://prometheus.io/docs/practices/histograms/) · [Grafana 告警评估](https://grafana.com/docs/grafana/latest/alerting/fundamentals/alert-rule-evaluation/) · [Grafana 文件 Provisioning](https://grafana.com/docs/grafana/latest/as-code/provision-resources/)
