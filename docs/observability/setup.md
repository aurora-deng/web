# 隔离启动与验收

`metrics` 档可用现有 Prometheus 3.13.5、Node Exporter 1.12.1 做慢 ACK 实验，调用链存实验 Observer。该实验示例用 18798，日常入口见部署记录。要复验 11 月 20 日的跨实例投递和真实 Collector/Tempo，使用双实例 `full` 档。[慢 ACK 启动与停止](ack-diagnosis.md#2-启动真实指标档) · [双实例实操](dual-instance-lab.md) · [日常启用记录](daily-rollout.md)

Envoy 的可选启动参数、端口、浏览器和黑盒实验见[网关隔离手册](envoy-gateway.md)。

双实例的独立数据目录、端口、恢复测试、Collector 与测试通知见[双实例实验](dual-instance-lab.md)。

只想查看页面时，可直接使用标准库预览。页面显式标记模拟数据，含告警、趋势、DAG、Agent 和设备样例。

```bash
python -m tools.observability.preview --data-dir build-function3/obs-preview
```

然后打开 `http://127.0.0.1:18785/`。重复启动须选择新的预览目录。下面是接入真实服务的步骤。

## 日常聊天室与苦情树

### 本轮 26 项业务埋点的隔离入口

2026-11-19 新埋点先在 [18797 函数页](http://127.0.0.1:18797/?view=profiles)验证，日常 18090 仍为上一版已授权程序。本轮入口与旧 18793 分开；详细覆盖和限制见[业务埋点讲义](business-instrumentation.md)。启动中的实验无需重复运行。

Windows 需要这个转发，保持终端运行，`Ctrl+C` 只关闭该转发：

```powershell
& 'C:\Windows\System32\OpenSSH\ssh.exe' -F NUL -o ExitOnForwardFailure=yes -N `
  -L 127.0.0.1:18797:127.0.0.1:18797 pikachu@192.168.239.135
```

隔离实验会限时结束。需要恢复时，先确认 18797 没有旧实验监听，再在 Linux 使用已有依赖和本轮构建；前台 `Ctrl+C` 会由实验所有者收尾其子进程和临时数据库：

```bash
BASE=/home/pikachu/phase11-data/function3/validation/observer-resources.GeaBv7
cd "$BASE/business-probes-src"
python3 -m tools.observability.lab --profile local --observer-port 18797 \
  --work-dir /home/pikachu/phase11-data/function3/observer-live-20261117/probes26 \
  --resume-data --exercise --duration 14400 \
  --postgres-prefix /home/pikachu/phase11-deps/postgresql/18.6 \
  --binary "$BASE/build-business-probes/webserver" \
  --http-trace-every 20 --ws-trace-every 100
```

`--resume-data` 保留监控数据，业务夹具数据库仍是新的临时测试环境；不会复用或改写原用户库。需要多项业务样本时，在另一终端执行埋点讲义中的 `business_probes_live.py`，只向这个隔离环境写入合成内容。

### 原 18090 和 18791

在当前虚拟机沿用原业务库的[日常服务入口](../ops/runtime/function3-runtime.md)。仅在服务停止后执行启动命令。

```bash
cd /home/pikachu/function3-src
python3 scripts/function3/runtime/service.py start
curl -fsS http://127.0.0.1:18090/health/ready
```

本次监控源码已放在下列目录。18791 停止后，可在另一个 Linux 终端用同一份 SQLite 继续采集 18090。Observer 和服务端都已启动时直接打开页面即可。

```bash
OBS=/home/pikachu/phase11-data/function3/observer-live-20261117
cd "$OBS/app"
python3 -m tools.observability.observer.server serve \
  --db "$OBS/daily/events.sqlite" --credentials "$OBS/daily/producers.json" \
  --port 18791 --local-scope daily --local-target 18090 \
  --local-target-pid "$(cat /home/pikachu/phase11-data/function3/runtime/webserver.pid)" \
  --local-otlp \
  --log-input-directory nebula-webserver=/home/pikachu/phase11-data/function3/runtime/structured-logs \
  --related-observer http://127.0.0.1:18793
```

Windows PowerShell 可同时转发日常监控和隔离压测监控。命令运行期间保留终端。

```powershell
& 'C:\Windows\System32\OpenSSH\ssh.exe' -F NUL -o ExitOnForwardFailure=yes -N `
  -L 127.0.0.1:18791:127.0.0.1:18791 -L 127.0.0.1:18793:127.0.0.1:18793 pikachu@192.168.239.135
```

- [日常业务性能](http://127.0.0.1:18791/?view=service)采集 18090 的 HTTP、连接、主机和进程指标。新窗口通常在两次抓取后出现速率。相对时间窗口以 Observer 的时钟为准，图表标签使用浏览器时区。[日常页面探针](http://127.0.0.1:18791/?view=boards&board=f6327fcf5dd6407cad96c76a)每 15 秒核对两个页面的 HTTP 状态和响应时间。[聊天室](http://192.168.239.135:18090/phase11/) · [苦情树](http://192.168.239.135:18090/phase11/tree/)
- [隔离实时压测](http://127.0.0.1:18793/?view=service)使用支持业务阶段埋点的构建。[研发实验](http://127.0.0.1:18793/?view=runs)可回看近七天的聊天和树读取摘要。

隔离实例结束后，恢复监控数据和下一轮测试可分别执行下面两段。压测脚本检查目标二进制和端口身份，使用隔离账号和果实。

```bash
OBS=/home/pikachu/phase11-data/function3/observer-live-20261117
cd "$OBS/app"
python3 -m tools.observability.lab --profile local --observer-port 18793 \
  --work-dir "$OBS/load" --resume-data --exercise --duration 14400 \
  --postgres-prefix /home/pikachu/phase11-deps/postgresql/18.6 \
  --binary /home/pikachu/phase11-data/function3/validation/observer-resources.GeaBv7/build-runtime-logging/webserver \
  --http-trace-every 20 --ws-trace-every 100
```

```bash
bash /home/pikachu/phase11-data/function3/observer-live-20261117/app/run-live-load.sh
```

当前隔离入口运行与日常功能开关对齐的完整构建，gRPC 使用独立回环端口。原 `build-make` 仍是精简隔离构建；比较实验会区分两者的功能配置，不能将精简程序直接替换日常完整程序。2026-11-19 已获得日常启用许可并完成切换；后续正常启动沿用授权标记开启业务采样。业务重启后，Observer 也应按上面的新 PID 重新启动，避免进程资源仍指向旧 PID。[原库验收与回退](daily-rollout.md)

## 七天保留和导图

「日志工作台」提供级别、组件和 Trace 筛选、连续跟随及当前结果 JSON 导出。结构化日志索引滚动保留七天；自有文本日志按 UTC 日轮转，保留今日及前六天，每天最多 8 MiB。后台定期清理，自定义的外部 Loki 使用其部署侧保留策略。[配置、组件与查询编辑器](logging.md)

Observer 至多保留七天的本机指标、桶快照、主机和进程、事件、链路、实验归档、已登记探针的采样及告警历史。运行中空闲也按分钟检查。正常抓取保留最近六小时的业务采样和最近一小时的资源采样，较老窗口每五分钟取该桶最后一次真实观测。链路数量及数据库空间仍有上限。由实验栈管理的 Prometheus 和 Tempo 同样配置为七天。另行接入的数据源由各自部署维护保留期限。

在有数据的时序图或「提交诊断」点击「导出 PNG」。时序图文件带标题、时间范围、时区和当前显示的曲线。导出的文件保存在浏览器的下载位置，适合留作 Obsidian 复盘。要留存时序数值，可在主面板的「检查 → 原始数据」下载 CSV。

## 连续查看实时曲线

2026-11-19 起，总览和业务性能页先显示已返回的指标，健康、版本、告警与故障标注分别更新。看到“附加查询中”可以继续查看曲线；告警失败时使用旁边的“重试失败的附加查询”，不会重新加载整个大盘。五秒刷新会复用未完成的附加请求，避免反复取消。放大面板只查询所需内容。[生命周期与边界](architecture.md#页面查询与生命周期)

「运行总览」和「业务性能」在自动刷新时复用当前面板、图表和图例按钮。可以隐藏一条曲线，保留鼠标悬停或键盘焦点，随后继续观察新增样本。选中固定时间窗、打开检查抽屉和拖选区间时暂停自动刷新；顶栏的「恢复实时」可回到当前时间。

查询失败的面板会显示「数据源不可用」，恢复后重新绘图。查看页顶的「最近样本」和采样延迟提示，确认当前值的时间。切换到其他栏目后，上一页的请求会取消，图表监听器会释放。

空白耗时图会进一步解释当前采样是否收到对应指标、有效采样中是否有新增调用，以及采集是否失败。数值为零时照常显示零。点「检查 → 查询详情」可查看原因、下一步检查建议和原查询。读取超过八秒会提示超时，可缩小时间范围后重试。

## 轻量实时档

复用已经构建且支持 PostgreSQL 的 NEBULA Release 二进制。Linux 虚拟机项目根目录执行：

```bash
ROOT=/home/pikachu/phase11-deps
RUN="/home/pikachu/phase11-data/function3/observer-live-$(date -u +%Y%m%dT%H%M%SZ)"
BINARY="$PWD/build-nebula/webserver"
python3 -m tools.observability.lab --profile local --work-dir "$RUN" \
  --postgres-prefix "$ROOT/postgresql/18.6" --binary "$BINARY" --exercise
```

将 `BINARY` 设为实际二进制位置。脚本启动新的隔离实例，每 5 秒抓取一次内部指标并读取 Linux 系统统计。`--exercise` 每分钟运行带 Trace Context 的真实登录后会话探针，用于生成延迟、服务事件及 Trace 样本。轻量档需要支持 libcurl 出站的 NEBULA 二进制才能收齐服务端 span。指标、链路、事件和实验存放于本轮 SQLite，遵守七天保留和容量限制，链路最多保留 24,000 个 span。Ctrl+C 退出后可用原目录加 `--resume-data` 启动，保留监控历史。业务库每次都由隔离脚本重新创建。

隔离脚本默认 `--http-trace-every 20`。每个工作线程对没有有效上游 `traceparent` 的已埋点 HTTP 请求记录首条，之后每 20 条记录一条。设为 `0` 时这类请求只保留出错入口，设为 `1` 时完整采集，适合短时间定位。每轮实际设置写入 `session.json`。直接启动二进制时用 `WEB_OBSERVABILITY_HTTP_TRACE_EVERY` 设置间隔，默认值为 `0`。HTTP 与 WebSocket 分别配置，固定维度计数和时延直方图继续覆盖所有已埋点请求。

Windows 若已在本机开模拟预览（端口 18785），使用 18787 转发 Linux 监控。保留这个窗口，浏览器打开 [真实业务性能](http://127.0.0.1:18787/?view=service)。

```powershell
C:\Windows\System32\OpenSSH\ssh.exe -N -L 127.0.0.1:18787:127.0.0.1:18785 pikachu@192.168.239.135
```

确认右上角显示「本机 · 隔离实时」。目前持续使用的新版隔离环境监听 18791，可将转发命令中的本地和远端端口都改为 18791。进入「链路追踪」打开一条会话探针。进入「研发实验」，将时间范围切到最近七天，即可查看导入的真实报告。新启动时可用 `--dashboards-dir config/observability/dashboards` 载入 Git 中的面板定义。实际运行函数、查询 P99 和验证来源的命令见[实操手册](explore-and-connect.md)。

在「服务目标与采集质量」选择当前时间窗，查看覆盖和事件量。新实例从第一对有效抓取开始计算增量，需等待所选窗口完整覆盖。不满六小时可查短窗影响，六小时预算仍等待覆盖。以下命令使用示例端口 18791，可替换为实际端口。

```bash
PORT=18791
END=$(date +%s)
curl -fsS "http://127.0.0.1:$PORT/api/v1/service-levels?from=$((END-300))&to=$END" | python3 -m json.tool
```

## 完整遥测档

2026-11-20 已复用下列组件完成真实双实例验收。复验跨实例消息、Collector 中断和本机通知时，走使用独立网络命名空间的双实例入口。下面的 `lab` 命令用于单上游交互式实验，适合通过 SSH 看趋势和 Trace。[双实例启动与验收](dual-instance-lab.md) · [技术用途与版本更新](sources.md)

用已有 Linux 工具链构建 NEBULA 的 Release 业务档。在[复现指南](../ops/validation/reproduce.md)的构建参数上增加 `WEBSERVER_ENABLE_CURL_CLIENT=ON` 与现有 libcurl 的 `WEBSERVER_CURL_ROOT`。该开关提供可选 OTLP HTTP 出站能力。业务回归档仍可按原指南运行。用下面的命令启动，构建目录用实测路径替换。

```bash
ROOT=/home/pikachu/phase11-deps
RUN="$HOME/phase11-data/function3/observability-$(date -u +%Y%m%dT%H%M%SZ)"
python3 -m tools.observability.lab --profile full \
  --work-dir "$RUN" --dependencies "$ROOT/observability" \
  --postgres-prefix "$ROOT/postgresql/18.6" --binary "$PWD/build-nebula/webserver"
```

脚本先检查已配置的可执行文件，再新建自己的 PostgreSQL 和业务端口。工具目录需要包含 `prometheus-3.13.5`（含 promtool）、`node_exporter-1.12.1`、`otelcol-0.165.0`、`tempo-3.0.3`。图表脚本随项目提供。使用经批准的官方制品。服务退出会停止本次管理的进程，保留输出目录。用启动终端的 Ctrl+C 停止。长稳使用 `--duration 86400 --exercise`，以临时账号每分钟查询一次真实会话。脚本守护监控目录总量，默认达到 1 GiB 或可用空间不足 1 GiB 时停止。新一轮使用新目录，旧目录留作复盘。

| Linux x86_64 制品 | 官方校验 |
|---|---|
| prometheus-3.13.5.linux-amd64.tar.gz | SHA-256 `0388822ace519c5a3a8ef556a541ce1b536e21d872f82deded465bd27a5a0699` · [Prometheus](https://prometheus.io/download/) |
| node_exporter-1.12.1.linux-amd64.tar.gz | SHA-256 `b51d8a76aa2a9156a55d501aca6276fae09e262259a5e4e831d2c2222f084e63` · [Node Exporter](https://prometheus.io/download/#node_exporter) |
| otelcol_0.165.0_linux_amd64.tar.gz | SHA-256 `78570dd4bd6570bc41eb327869ef913bbf91b87dd8170ebebbf43bec71f6370d` · [Collector](https://github.com/open-telemetry/opentelemetry-collector-releases/releases/expanded_assets/v0.165.0) |
| tempo_3.0.3_linux_amd64.tar.gz | SHA-256 `7ac4f095dfaee3ba77dbdbb23ebd9f8a021cb6895c3602244ee3bf2787d8ceba` · [Tempo](https://github.com/grafana/tempo/releases/expanded_assets/v3.0.3) |
| alertmanager-0.34.2.linux-amd64.tar.gz | SHA-256 `17ee14acd0f6ba6e25a436ff223d9c17ddd5cf484f543ad12f0932eaa89ba186` · [Alertmanager](https://github.com/prometheus/alertmanager/releases/expanded_assets/v0.34.2) |

复用依赖目录中版本匹配的组件。Alertmanager 是可选通知组件，实验通过 `--with-alertmanager` 启用。Tempo 3.0.3 以单进程使用本地存储，七天保留写在 `backend_worker.compaction.block_retention`。[Tempo 3.0 迁移说明](https://grafana.com/docs/tempo/latest/set-up-for-tracing/setup-tempo/migrate-to-3/)

Collector 归档约 45.5 MiB，解包后程序和许可约 172.2 MiB。为遵守 Linux 上的 180 MiB 安装预算，用输入流接收已核验的官方归档，安装器在激活程序前再次检查整个归档的 SHA-256。该归档没有附许可文件，传入同一标签的官方 `LICENSE`，安装器会检查其摘要。[Collector 官方同版本许可](https://github.com/open-telemetry/opentelemetry-collector-releases/blob/v0.165.0/LICENSE)

```bash
python3 -m tools.observability.install_pipeline --approved collector \
  --asset-stdin --license-file "$COLLECTOR_LICENSE" < "$COLLECTOR_ARCHIVE"
```

Windows PowerShell 中建立隧道，再打开本机浏览器的 `http://127.0.0.1:18785/`。

```powershell
C:\Windows\System32\OpenSSH\ssh.exe -N -L 127.0.0.1:18785:127.0.0.1:18785 pikachu@192.168.239.135
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

可用真实隔离数据在 Windows 运行 `node tests/project/observer_browser.cjs`。默认访问本机 18787，脚本要求已写入真实会话探针、至少两个可比较实验及标记为模拟的扩展夹具。

`wire` 使用测试 OTLP HTTP 接收器检查真实业务导出和父子关联。完整栈使用 `stack` 验证。实验文件可以用 `python3 -m tools.observability.observer.server import-run --db "$RUN/events.sqlite" REPORT.json` 导入。只有匹配配置、负载、夹具和运行环境，且至少 1,000 条成功 ACK 的实验才允许比较。注意保留实验持续时段及 SHA，旧报告缺字段时保留原样。

下面的告警实验创建一次性 PostgreSQL，建立真实查询锁等待和 5xx。它保留告警变化和原始计数，通常在六分钟内结束。

```bash
OUT="$HOME/phase11-data/function3/observer-alert-$(date -u +%Y%m%dT%H%M%SZ)"
python3 -m tools.observability.alert_lab --binary "$BINARY" \
  --postgres-prefix "$ROOT/postgresql/18.6" --output "$OUT"
```

见 `$OUT/report.json` 的故障与告警转换，以及 `$OUT/alerts.sqlite` 的 `metric_samples`。复习双窗口时可先看[真实验收时间窗](validation.md#服务目标与采集质量的真实验证)。

用 `python3 -m tools.observability.experiments calibrate`、`overhead` 或 `capacity` 生成新记录。三个入口均要求 `--binary`、`--postgres-prefix` 和全新的 `--output`。每个变体复用脚本独立创建的临时库，读取运行二进制自身的构建标识，只在隔离环境运行。配对校准和开销对照需要已安装的 `wrk`。`WEB_OBSERVABILITY=1` 启用埋点，`WEB_OBSERVABILITY_LOG=off|error|info` 在启动时选择结构化日志级别。

用下面的命令标记故障时间。导入的实验报告自动生成开始和结束标线。持续运行脚本按分钟记录存活、就绪、队列、丢弃、RSS、文件描述符和占用在 `soak.ndjson`。

```bash
python3 -m tools.observability.observer.server mark --credentials "$RUN/producers.json" \
  --run run-01 --id incident-01 --scenario pg-disconnect --state fault
```
