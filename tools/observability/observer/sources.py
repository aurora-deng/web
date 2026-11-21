"""Fixed queries and bounded adapters for the two monitoring backends."""
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from .operation_catalog import FUNCTIONS, FUNCTION_CALLS, FUNCTION_IN_FLIGHT

STAGES = ("queue", "pool", "before_commit", "commit", "ack_ready")
HTTP_ROUTES = {"http": "conversations", "history": "history"}
COUNTERS = {
    "http": "webserver_http_requests_total",
    "outbox": "webserver_phase11_outbox_published_total",
    "auth_rejected": "webserver_auth_rejected_total",
    "origin_rejected": "webserver_origin_rejected_total",
    "rate_limited": "webserver_rate_limited_total",
    "db_failed": "webserver_phase11_db_executor_failed_total",
    "outbox_failed": "webserver_phase11_outbox_failures_total",
    "log_written": "nebula_business_log_written_total",
    "log_dropped": "nebula_business_log_dropped_total",
    "log_queue_full": "nebula_business_log_queue_full_total",
}
GAUGES = {
    "ws": "webserver_websocket_online", "sse": "webserver_sse_online",
    "queue": "webserver_phase11_db_executor_queued",
    "pool": "webserver_phase11_postgres_pool_in_use",
}
SIGNALS = {"scrape": 'up{job="nebula"}', "alive": "nebula_probe_live",
           "ready": "nebula_probe_ready", "monitor": 'up{job="observer"}'}
HTTP_RESPONSES = "nebula_business_http_responses_total"
DB_MAX_US = "webserver_business_database_microseconds_max"
ERROR_PERCENT = ('100 * sum by (bc, app, instance) (rate('+HTTP_RESPONSES+'{status_class="5xx"}[5m]))'
                 ' / sum by (bc, app, instance) (rate('+HTTP_RESPONSES+'[5m]))')
BUSINESS_ALERTS = {
    "NebulaHttpHighErrors": {"title":"业务 HTTP 5xx 比例 > 5%","expr":ERROR_PERCENT+" > 5",
                             "threshold":5,"unit":"%","window":"5m","pending":0,
                             "guide":"口径是共用业务适配器完成的 HTTP 响应，检查 5xx、请求样本量和数据库就绪。"},
    "NebulaDatabaseMaxSlow": {"title":"数据库历史最大耗时 > 2 秒","expr":DB_MAX_US+" > 2000000",
                              "threshold":2000000,"unit":"μs","window":"5m","pending":300,
                              "guide":"这是进程生命周期最大值。一次尖峰即可维持高值，对照 P99 与请求样本。"},
}

PANELS = {
    "http_5xx_pct": ("业务 HTTP 5xx 比例 · 5分钟","%",ERROR_PERCENT,"分母是同批已计数的业务适配器响应。"),
    "db_lifetime_max": ("数据库操作进程最大耗时","ms",DB_MAX_US+"/1000","进程启动以来的最大值，对瞬时尖峰敏感。"),
    "db_operation_p99": ("数据库操作 P99", "ms",
        '1000*histogram_quantile(0.99,sum by(instance,le)(rate(nebula_database_operation_seconds_bucket{job="nebula"}[5m])))',
        "五分钟直方图，历史最大值保留在独立诊断指标。"),
    "realtime_pending_age": ("实时投递周期最老待处理事件", "s", "nebula_realtime_pending_age_seconds",
        "成功轮询观察到的最老待投递年龄。空队列显示 0，查询失败时不报数。"),
    "telemetry_drop_rate": ("业务 Trace 丢弃速率", "段/秒", 'rate(nebula_telemetry_dropped_total{job="nebula"}[5m])',
        "区分业务请求成功与可观测性数据丢失。"),
    "http": ("HTTP 请求速率", "次/秒", f"rate({COUNTERS['http']}[2m])", "服务收到的 HTTP 请求，包含探针和抓取流量。"),
    "ws": ("WebSocket 连接", "连接", "webserver_websocket_online", "当前在线连接。"),
    "sse": ("SSE 连接", "连接", "webserver_sse_online", "当前在线连接。"),
    "queue": ("数据库任务排队", "项", "webserver_phase11_db_executor_queued", "结合队列等待时间和连接池使用量判断。"),
    "pool": ("连接池使用量", "连接", "webserver_phase11_postgres_pool_in_use", "借用中的连接数。"),
    "outbox": ("Outbox 投递速率", "条/秒", f"rate({COUNTERS['outbox']}[2m])", "与聊天持久化 ACK 分开观察。"),
    "cpu": ("主机 CPU 使用率", "%", '100*(1-avg by(instance)(rate(node_cpu_seconds_total{mode="idle"}[2m])))', "主机总体使用率。"),
    "memory": ("主机内存使用率", "%", "100*(1-node_memory_MemAvailable_bytes/node_memory_MemTotal_bytes)", "以可用内存为准。"),
    "disk": ("根文件系统可用空间", "GiB", 'node_filesystem_avail_bytes{mountpoint="/",fstype!~"rootfs|overlay"}/1073741824', "查看监控保留空间。"),
    "threads": ("NEBULA 进程线程", "线程", "nebula_probe_target_threads", "读取被监控 NEBULA 进程的 Linux 线程数。"),
    "network_tx": ("主机网络发送", "B/秒", 'sum by(instance)(rate(node_network_transmit_bytes_total{device!="lo"}[2m]))', "主机非回环网卡速率，包含其他进程的流量。"),
    "network_rx": ("主机网络接收", "B/秒", 'sum by(instance)(rate(node_network_receive_bytes_total{device!="lo"}[2m]))', "主机非回环网卡速率，包含其他进程的流量。"),
    "device": ("设备窗口样本数", "帧", "nebula_observer_device_window_samples", "最近一个降采样窗口的帧数，按模拟来源固定映射。"),
    "gateway_up": ("入口指标抓取", "", 'up{job="gateway"}', "Prometheus 对 Envoy 管理端指标接口的抓取状态。"),
    "gateway_connections": ("入口活动连接", "连接", 'sum by(instance)(envoy_http_downstream_cx_active{job="gateway"})',
                            "Envoy 原生连接数，包含活跃与空闲连接。"),
    "gateway_upstream_connect_p99": ("上游建连 P99", "ms",
        'histogram_quantile(0.99,sum by(instance,le)(rate(envoy_cluster_upstream_cx_connect_ms_bucket{job="gateway",envoy_cluster_name="nebula_application"}[5m])))',
        "Envoy 按新建连接统计；复用连接的请求不会重复计入。"),
    "gateway_rate": ("入口完成请求速率", "次/秒", "sum by(instance,route)(rate(nebula_gateway_access_total[2m]))",
                     "来自有界业务访问日志；WS/SSE 在连接结束后计数，/fast 单独由基准程序统计。"),
    "gateway_log_pending": ("入口待采集日志", "B", "nebula_gateway_log_pending_bytes", "入口日志文件中尚未处理的字节。"),
    "gateway_failures": ("入口观测到的 5xx", "次/秒",
                         'sum by(instance,origin)(rate(nebula_gateway_access_total{status_class="5xx"}[2m]))',
                         "由上游设置的状态码记作 upstream；代理本地返回的错误记作 gateway。"),
}
for stage, title in (("request", "入口请求总耗时"), ("upstream_header", "上游响应头耗时"),
                     ("upstream_response", "上游响应耗时")):
    PANELS[f"gateway_{stage}_p99"] = (f"{title} P99", "ms",
        '1000*histogram_quantile(0.99,sum by(instance,le)(rate(nebula_gateway_http_seconds_bucket{stage="'+stage+'"}[5m])))',
        "只统计结束的普通 HTTP 请求，排除 WebSocket/SSE 长连接。分位数不能相减。")
for protocol, label in (("sse", "SSE"), ("websocket", "WebSocket")):
    PANELS[f"gateway_wasm_{protocol}_p99"] = (f"{label} 建连响应 P99 · Wasm", "ms",
        'histogram_quantile(0.99,sum by(instance,le)(rate({__name__=~".*nebula_'+protocol+
        '_handshake_ms_bucket",job="gateway"}[5m])))',
        "仅在已加载插件后采集。响应头时刻与客户端收到首个 SSE 事件分别测量。")
for key, title in (("log_written", "C++ 日志写入"), ("log_dropped", "C++ 日志丢弃"), ("log_queue_full", "C++ 日志队列满"),
                   ("auth_rejected", "鉴权拒绝"), ("origin_rejected", "来源限制"),
                   ("rate_limited", "限流拒绝"), ("db_failed", "数据库任务失败"), ("outbox_failed", "Outbox 投递失败")):
    PANELS[key] = (title, "次/秒", f"rate({COUNTERS[key]}[2m])", "独立事件计数，不能相加作为全站 HTTP 失败数。")
for prefix, route in HTTP_ROUTES.items():
    histogram = "nebula_http_handler_seconds"
    label = '{route="' + route + '"}'
    count = f"sum by(instance)(rate({histogram}_count{label}[5m]))"
    title = "会话列表" if prefix == "http" else "消息历史"
    guide = "业务 handler 到响应就绪，使用五分钟窗口。有请求样本时才显示延迟。"
    PANELS[prefix + "_rate"] = (title + "处理速率", "次/秒", count, guide)
    PANELS[prefix + "_avg"] = (title + "平均耗时", "ms",
        f"1000*sum by(instance)(rate({histogram}_sum{label}[5m])) / ({count} > 0)", guide)
    for percentile in (50, 90, 95, 99):
        PANELS[f"{prefix}_p{percentile}"] = (f"{title} P{percentile}", "ms",
            f"1000*histogram_quantile({percentile / 100},sum by(instance,le)(rate({histogram}_bucket{label}[5m])))", guide)
for stage, title in zip(STAGES, ("任务排队", "取连接", "提交前累计", "COMMIT", "ACK 就绪累计")):
    PANELS["chat_" + stage] = (title + " P99", "ms",
        '1000*histogram_quantile(0.99,sum by(instance,le)(rate(nebula_chat_stage_seconds_bucket{stage="' + stage + '"}[5m])))',
        "服务端处理阶段。累计值之间有重叠，分位数不能相减。")
for prefix, name in FUNCTIONS.items():
    label='{function="'+name+'"}'
    count='sum by(instance)(rate(nebula_function_seconds_count'+label+'[5m]))'
    PANELS[prefix+"_rate"]=(name+" 调用速率","次/秒",count,"五分钟窗口内的进出函数样本。")
    PANELS[prefix+"_avg"]=(name+" 平均墙钟耗时","ms",
        '1000*sum by(instance)(rate(nebula_function_seconds_sum'+label+'[5m])) / ('+count+' > 0)',
        "包含子调用和等待。按样本计数加权。")
    for percentile in (95,99):
        PANELS[f"{prefix}_p{percentile}"]=(f"{name} 墙钟 P{percentile}","ms",
            f'1000*histogram_quantile({percentile/100},sum by(instance,le)(rate(nebula_function_seconds_bucket{label}[5m])))',
            "直方图估计值，包含子调用和等待。")
    PANELS[prefix+'_error_rate']=(name+' 失败速率','次/秒',
        'sum by(instance)(rate('+FUNCTION_CALLS+'{function="'+name+'",outcome="error"}[5m]))',
        '业务拒绝和执行异常都计入 error；不等同 HTTP 5xx。取消和超时单列。')
    PANELS[prefix+'_in_flight']=(name+' 正在执行','项',FUNCTION_IN_FLIGHT+label,
        '已进入但未结束的操作。异步模型请求到最终完成、取消或超时才减一。')


class BackendError(Exception):
    pass


class NoTrace(Exception):
    pass


class Sources:
    def __init__(self, prometheus, tempo, probe="http://127.0.0.1:18786"):
        self.prometheus = prometheus
        self.tempo = tempo
        self.probe = probe

    def fetch(self, base, path, params=None, trace=False):
        url = base.rstrip("/") + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        try:
            with urllib.request.urlopen(url, timeout=2) as response:
                data = response.read(2 * 1024 * 1024 + 1)
            if len(data) > 2 * 1024 * 1024:
                raise BackendError("查询结果过大")
            result = json.loads(data)
            if not isinstance(result, dict):
                raise BackendError("数据源返回格式异常")
            return result
        except urllib.error.HTTPError as error:
            if trace and error.code == 404:
                raise NoTrace from error
            raise BackendError("数据源返回错误") from error
        except (OSError, ValueError) as error:
            raise BackendError("数据源不可用") from error

    def prom(self, path, params=None):
        data = self.fetch(self.prometheus, "/api/v1/" + path, params)
        if data.get("status") != "success" or "data" not in data:
            raise BackendError("Prometheus 查询失败")
        return data["data"]

    def ranges(self,panels,start,end,step,diagnostics):
        from .prometheus_queries import ranges
        return ranges(self,panels,start,end,step,diagnostics)

    def query_promql(self,expr,start,end,step,kind):
        return self.prom('query' if kind=='instant' else 'query_range',
            {'query':expr,'time':end} if kind=='instant' else
            {'query':expr,'start':start,'end':end,'step':step})

    def service_levels(self, start, end, instance=""):
        from .prometheus_service_levels import report
        return report(self, start, end, instance)

    def instrumentation(self,start,end):
        query='{__name__=~"nebula_(http_handler|chat_stage|function)_seconds_count|nebula_function_calls_total"}'
        rows=self.prom('query',{'query':query,'time':end}).get('result',[])
        families=sorted({row['metric'].get('__name__','').removesuffix('_count') for row in rows
                         if row['metric'].get('__name__','').endswith('_seconds_count')})
        observed=sorted({row['metric']['function'] for row in rows if
                         row['metric'].get('__name__')=='nebula_function_seconds_count' and
                         row['metric'].get('function') in FUNCTIONS.values()})
        instances=sorted({row['metric']['instance'] for row in rows if
                          row['metric'].get('__name__')=='nebula_function_seconds_count' and
                          row['metric'].get('instance')})
        available='nebula_function_seconds' in families
        return {'status':'ready' if available else 'missing-instrumentation','available':available,
            # A display label must never become an instance selector. Multiple
            # targets retain their real identities in the returned time series.
            'requiredMetric':'nebula_function_seconds','target':{
                'instance':instances[0] if len(instances)==1 else None,
                'instances':instances,'multiple':len(instances)>1},
            'sampleAt':max((float(row['value'][0]) for row in rows),default=None),
            'observedFunctions':observed,'observedHistogramFamilies':families,
            'functionOutcomes':any(row['metric'].get('__name__')==FUNCTION_CALLS for row in rows),
            'guide':'已通过 Prometheus 采到函数直方图；延迟只在所选窗口有实际调用时显示。' if available else
                    '所选时刻未采到函数直方图，请检查目标版本、抓取状态和时间范围。'}

    def trace(self, trace_id):
        return self.fetch(self.tempo, "/api/v2/traces/" + trace_id, trace=True)

    def search_traces(self, start, end, operation, failed, limit, service=""):
        conditions = []
        if operation:
            conditions.append("name = " + json.dumps(operation))
        if service:
            conditions.append("resource.service.name = " + json.dumps(service))
        if failed:
            conditions.append("status = error")
        params = {"start": start//10**9, "end": end//10**9, "limit": min(limit,100)}
        if conditions:
            params["q"] = "{ " + " && ".join(conditions) + " }"
        rows = self.fetch(self.tempo, "/api/search", params).get("traces", [])
        items = []
        for row in rows[:limit]:
            if not re.fullmatch(r"[0-9a-f]{16,32}", row.get("traceID", "")):
                continue
            item = {"traceId": row["traceID"].zfill(32), "name": row.get("rootTraceName","未知操作"),
                    "service": row.get("rootServiceName","未知服务"), "startedAt": str(row["startTimeUnixNano"]),
                    "durationMs": row.get("durationMs",0), "error": True if failed else None}
            if service:
                # TraceQL matches the function and service on the same span. A
                # backend may omit spans from its search response. Keep its ID
                # for detail lookup without passing trace duration off as a span.
                sets = row.get("spanSets") or [row.get("spanSet") or {}]
                match = next((span for group in sets for span in group.get("spans", [])
                              if re.fullmatch(r"[0-9a-f]{16}", span.get("spanID", ""))), {})
                duration = str(match.get("durationNanos", ""))
                item.update(name=operation, service=service, spanId=match.get("spanID"),
                            startedAt=str(match.get("startTimeUnixNano", item["startedAt"])),
                            durationMs=int(duration)/10**6 if duration.isdecimal() else None)
            items.append(item)
        return items
