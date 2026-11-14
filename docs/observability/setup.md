# 隔离启动与验收

只想查看页面时，可直接使用标准库预览。页面显式标记模拟数据，含告警、趋势、DAG、Agent 和设备样例。

```bash
python -m tools.observability.preview --data-dir build-function3/obs-preview
```

然后打开 `http://127.0.0.1:18785/`。重复启动须选择新的预览目录。下面是接入真实服务的步骤。

用已有 Linux 工具链构建 NEBULA 的 Release 业务档。在[复现指南](../ops/validation/reproduce.md)的构建参数上增加 `WEBSERVER_ENABLE_CURL_CLIENT=ON` 与现有 libcurl 的 `WEBSERVER_CURL_ROOT`。该开关提供可选 OTLP HTTP 出站能力。业务回归档仍可按原指南运行。用下面的命令启动，构建目录用实测路径替换。

```bash
ROOT=/home/pikachu/phase11-deps
RUN="$HOME/phase11-data/function3/observability-$(date -u +%Y%m%dT%H%M%SZ)"
python3 -m tools.observability.lab \
  --work-dir "$RUN" --dependencies "$ROOT/observability" \
  --postgres-prefix "$ROOT/postgresql/18.6" --binary "$PWD/build-nebula/webserver"
```

脚本先检查已配置的可执行文件，再新建自己的 PostgreSQL 和业务端口。工具目录需要包含 `prometheus-3.13.5`（含 promtool）、`node_exporter-1.12.1`、`otelcol-0.165.0`、`tempo-3.0.3`、`echarts-6.1.0/echarts.min.js`。使用经批准的官方制品。服务退出会停止本次管理的进程，保留输出目录。用启动终端的 Ctrl+C 停止。长稳使用 `--duration 86400 --exercise`，以临时账号每分钟查询一次真实会话。脚本守护监控目录总量，默认达到 1 GiB 或可用空间不足 1 GiB 时停止。新一轮使用新目录，旧目录留作复盘。

| Linux x86_64 制品 | 官方校验 |
|---|---|
| prometheus-3.13.5.linux-amd64.tar.gz | SHA-256 `0388822ace519c5a3a8ef556a541ce1b536e21d872f82deded465bd27a5a0699` · [Prometheus](https://prometheus.io/download/) |
| node_exporter-1.12.1.linux-amd64.tar.gz | SHA-256 `b51d8a76aa2a9156a55d501aca6276fae09e262259a5e4e831d2c2222f084e63` · [Node Exporter](https://prometheus.io/download/#node_exporter) |
| otelcol_0.165.0_linux_amd64.tar.gz | SHA-256 `78570dd4bd6570bc41eb327869ef913bbf91b87dd8170ebebbf43bec71f6370d` · [Collector](https://github.com/open-telemetry/opentelemetry-collector-releases/releases/expanded_assets/v0.165.0) |
| tempo_3.0.3_linux_amd64.tar.gz | SHA-256 `7ac4f095dfaee3ba77dbdbb23ebd9f8a021cb6895c3602244ee3bf2787d8ceba` · [Tempo](https://github.com/grafana/tempo/releases/expanded_assets/v3.0.3) |
| echarts 6.1.0 | 发布包 `dist.integrity` 校验后提取 `dist/echarts.min.js`、LICENSE、NOTICE · [ECharts 官方分发说明](https://echarts.apache.org/handbook/en/basics/download/) |

Windows PowerShell 中建立隧道，再打开本机浏览器的 `http://127.0.0.1:18785/`。

```powershell
ssh -N -L 18785:127.0.0.1:18785 pikachu@192.168.239.135
```

另开 Linux 终端，从项目根目录执行：

```bash
python3 -m tools.observability.verify stack --run-dir "$RUN"
```

它请求真实会话 API，验证 Tempo 返回父子 span，并用四个样例核对 DAG、失败分支、Agent、设备数据。登录凭据只用于临时测试。大盘「扩展遥测」可进入 DAG/Agent 的详细链路。Prometheus 会轮询新目标，初始暂无数据时等两个抓取周期再检查。

## 分项验证

```bash
python3 tests/project/test_observability.py
python3 -m tools.observability.verify events --output "$PWD/build-function3/obs-events"
python3 -m tools.observability.verify wire --binary "$PWD/build-nebula/webserver" \
  --postgres-prefix "$ROOT/postgresql/18.6" --output "$PWD/build-function3/obs-wire"
```

`wire` 使用测试 OTLP HTTP 接收器检查真实业务导出和父子关联。完整栈使用 `stack` 验证。实验文件可以用 `python3 -m tools.observability.observer.server import-run --db "$RUN/events.sqlite" REPORT.json` 导入。只有匹配配置、负载、夹具和运行环境，且至少 1,000 条成功 ACK 的实验才允许比较。注意保留实验持续时段及 SHA，旧报告缺字段时保留原样。

用 `python3 -m tools.observability.experiments calibrate`、`overhead` 或 `capacity` 生成新记录。三个入口均要求 `--binary`、`--postgres-prefix` 和全新的 `--output`。每个变体复用脚本独立创建的临时库，读取运行二进制自身的构建标识，只在隔离环境运行。配对校准和开销对照需要已安装的 `wrk`。`WEB_OBSERVABILITY=1` 启用埋点，`WEB_OBSERVABILITY_LOG=off|error|info` 在启动时选择结构化日志级别。

用下面的命令标记故障时间。导入的实验报告自动生成开始和结束标线。持续运行脚本按分钟记录存活、就绪、队列、丢弃、RSS、文件描述符和占用在 `soak.ndjson`。

```bash
python3 -m tools.observability.observer.server mark --credentials "$RUN/producers.json" \
  --run run-01 --id incident-01 --scenario pg-disconnect --state fault
```
