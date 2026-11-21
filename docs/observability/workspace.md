# 自定义仪表盘、数据源与分布式排障

1. 打开「数据源 → 添加数据源」。选择 Prometheus API 并填写服务基地址，或选择文本指标并填写完整 /metrics URL。HTTP 可用性填写健康检查地址。Tempo、Jaeger v3、Loki 和 Pyroscope 填各自 HTTP 查询服务的基地址。
2. 点击「测试连接」。文本指标可以从本次发现的指标中选择采样项。NEBULA 隔离实时档提供当前 /metrics 的快捷填充，复用服务端采集凭据。
3. 打开「自定义仪表盘」，从模板创建，或新建空白仪表盘后添加面板。每个面板支持四条查询，面板间可选择不同数据源。可修改图形、单位、颜色、宽度和排序，并检查读数、导出 CSV 或仪表盘 JSON。
4. 接入同类的新服务通过配置完成。Prometheus 负责采集目标发现，Observer 查询指标与标签。HTTP 健康探测显示状态、可用性及耗时。直接 /metrics 抓取所选指标的原始值，保留一天；速率、分位数和跨实例聚合使用 PromQL。

隔离业务端口由启动脚本分配。重启后用「填入当前 NEBULA /metrics」核对指标源，用同一个端口更新就绪探测，再分别测试并等待历史读数出现。编辑已有源时，面板继续引用原 ID；变更目标 URL 会重新开始该源的历史采集。需要保留旧数据时，应先归档旧样本。实际重新绑定及压测见[验收记录](validation.md#重新绑定数据源后的在跑实例压测)。

模板包括 NEBULA 聊天、主机资源、HTTP 可用性、Node Exporter、Process Exporter、抓取目标、Kubernetes、容器 cAdvisor、微服务 RED、PostgreSQL、Redis 和 Envoy。Envoy 通用模板从 Prometheus 按 `job` 和实例读取抓取状态、连接和请求。NEBULA 隔离网关另有带业务 Trace 关联的[专用大盘](envoy-gateway.md#3-从请求走到业务和数据库)。Grafana JSON 可导入部分 Prometheus 表达式和面板类型，预览标出跳过数量。

Prometheus 面板支持资源标签 cluster、namespace、service、pod、instance、node、job。可以在正则标签选择器中用 ${instance} 等参数，打开「加载标签」取得候选值，再「应用筛选」。值按字面量转义，全部展开成匹配任意值。采集器应已附加对应标签。Kubernetes 模板需要 kube-state-metrics，微服务 RED 模板需要匹配命名与单位的 span 指标。

## 从异常指标追到服务与日志

1. 在指标面板确认时段、实例、命名空间和服务。轻量档可从「服务目标与采集质量」查受影响的请求数，再跟随同窗指标和 Trace 链接。用「固定这次观测」保留时间窗。
2. 在「分布式排障」选 Tempo 并填写 TraceQL，或选 Jaeger v3 并填写准确服务名，两者都可按已知 Trace ID 下钻。隔离档也可选择「当前 NEBULA 链路」，看真实探针和数据库调用。
3. 对照失败 span、耗时和本次请求的跨服务调用边，在瀑布图中按服务、节点或 Pod 筛选。最长 span 包含下游耗时，需继续展开才能定位。
4. 选择 Loki，填写日志流选择器。页面使用当前 Trace ID 和 span 时间窗查询同一次请求的日志。应用日志须携带 Trace ID。通过瀑布图链接打开同一时间窗的业务指标或主机进程。
5. 到「函数与内核剖析」选 Pyroscope。先加载实际存在的 Profile Type，再用与服务一致的标签查火焰图。函数墙钟、本机堆和 syscall 实验的口径见[资源排障手册](resources.md)。

瀑布图支持部分链路标识，读取 OTel service.name 和 Kubernetes、host 资源标签。轻量档只显示已上报的字段。跨多请求的拓扑聚合、Kubernetes 现场、生产授权和容量仍需实际部署验收。

要串联两台或以上实例，接入时同时核对服务名、实例标签、主机或 Pod 和时区。Trace ID 可以精确匹配一条日志。同一时间段的 CPU 或候选慢 Trace 需要再用实例与请求路径验证。通过「数据源」连接 Loki 和 Pyroscope 后，用真实业务行为确认日志和剖析数据到达。

## 运维配置

服务地址由 Linux Observer 访问。默认允许回环地址；集群或局域网在启动时可用 --source-network 指定允许的 CIDR。新增同网段目标只操作「数据源」页面。请求拒绝云元数据、链路本地及本机 Observer 端口，不跟随跳转。

需要认证网关时提供 --source-auth-file 指向运维私有 JSON。以精确 URL 为键，值可含 bearer_file（相对于配置文件的令牌路径）及 tenant（映射到 X-Scope-OrgID）。浏览器和仪表盘文件不含凭据，带凭据的远端要求 HTTPS。多人生产部署仍需审批与角色访问。

## 验证入口

运行 python tests/project/test_observability.py 验证回环数据源协议和安全边界。运行 node tests/project/observer_workspace_browser.cjs 复验真实 /metrics URL、新建与编辑、多个查询、布局、刷新、导出和移动端。新测试实例监听 18789，旧 18787 对应此前独立长稳。固定 URL 源如果指向临时业务进程，重启后在页面更新实际端口。

启动命令沿用[隔离实时档](setup.md#轻量实时档)，用 `--observer-port 18789` 选择独立端口。Windows 若没有对应隧道，可运行：

```powershell
C:\Windows\System32\OpenSSH\ssh.exe -N -L 127.0.0.1:18789:127.0.0.1:18789 pikachu@192.168.239.135
```

[Grafana 创建仪表盘](https://grafana.com/docs/grafana/latest/visualizations/dashboards/build-dashboards/create-dashboard/) · [Grafana 模板导入](https://grafana.com/docs/grafana/latest/visualizations/dashboards/build-dashboards/import-dashboards/) · [Prometheus HTTP API](https://prometheus.io/docs/prometheus/latest/querying/api/) · [OpenTelemetry 可观测性](https://opentelemetry.io/docs/concepts/observability-primer/) · [Tempo API](https://grafana.com/docs/tempo/latest/api_docs/) · [Loki API](https://grafana.com/docs/loki/latest/reference/loki-http-api/) · [Datadog 服务依赖参考](https://docs.datadoghq.com/tracing/services/service_map/)
