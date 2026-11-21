"""Prometheus rules for a labeled and isolated NEBULA stack."""

SCOPE = "bc,app,instance"
MIN_HTTP_5M = 100
MIN_DB_5M = 200


def rule(name, title, expr, pending, severity, guide, *, window=None, unit=""):
    return {"alert": name, "expr": expr, "for": pending, "labels": {"severity": severity},
            "annotations": {"title": title, "expression": expr, "guide": guide, "window": window or pending, "unit": unit}}


def prometheus_alerts(extra_jobs=()):
    grouped = f"sum by ({SCOPE})"
    responses = 'nebula_business_http_responses_total{job="nebula"}'
    errors = 'nebula_business_http_responses_total{job="nebula",status_class="5xx"}'
    observed = f"{grouped}(increase({responses}[5m]))"
    fraction = f"{grouped}(increase({errors}[5m])) / {observed}"
    db_total = f'{grouped}(increase(nebula_database_operation_seconds_count{{job="nebula"}}[5m]))'
    db_bins = f'sum by ({SCOPE},le)(rate(nebula_database_operation_seconds_bucket{{job="nebula"}}[5m]))'
    targets = "|".join(("nebula", "probe", "observer", "node", *extra_jobs))
    return [
        rule("NebulaUnreachable", "业务进程不可达", 'nebula_probe_live{job="probe"} == 0', "30s", "critical",
             "核对对应实例的存活探针、进程状态和节点变更。"),
        rule("NebulaUnready", "业务尚未就绪", 'nebula_probe_ready{job="probe"} == 0 and nebula_probe_live{job="probe"} == 1',
             "30s", "critical", "检查 PostgreSQL、连接池和迁移状态。"),
        rule("NebulaScrapeFailed", "指标采集失败", f'up{{job=~"{targets}"}} == 0', "30s", "warning",
             "核对具体抓取目标、网络和采集凭据。"),
        rule("NebulaQueueBacklog", "数据库执行器持续排队", 'webserver_phase11_db_executor_queued{job="nebula"} > 0',
             "1m", "warning", "对照同实例排队 P99、连接池等待和事务。"),
        rule("NebulaHttpHighErrors", "近期 HTTP 5xx 比例超过 5%", f'(100 * ({fraction}) > 5) and on ({SCOPE}) ({observed} >= {MIN_HTTP_5M})',
             "1m", "critical", "回看该实例五分钟的样本数、HTTP 5xx 日志和调用链。", window="5m", unit="%"),
        rule("NebulaDatabaseSlowP99", "近期数据库操作 P99 超过 250 ms",
             f'(histogram_quantile(0.99,{db_bins}) > 0.25) and on ({SCOPE}) ({db_total} >= {MIN_DB_5M})',
             "2m", "warning", "检查近期真实样本和慢 Trace，区分锁等待、连接池和磁盘。", window="5m", unit="s"),
        rule("NebulaRealtimePendingOld", "实时投递积压超过 15 秒", 'nebula_realtime_pending_age_seconds{job="nebula"} > 15',
             "1m", "warning", "核对当前投递循环、数据库、队列失败和实例游标。", unit="s"),
        rule("NebulaTelemetryDropped", "业务 Trace 出现丢弃", 'increase(nebula_telemetry_dropped_total{job="nebula"}[5m]) > 0',
             "30s", "warning", "检查导出器队列、Collector 状态和采样率。业务 ACK 有独立计数。", window="5m", unit="段"),
    ]
