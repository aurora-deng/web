"""Fixed queries and bounded adapters for the two monitoring backends."""
import json
import urllib.error
import urllib.parse
import urllib.request

STAGES = ("queue", "pool", "before_commit", "commit", "ack_ready")
PANELS = {
    "http": ("HTTP 请求速率", "次/秒", "rate(webserver_http_requests_total[2m])", "含健康探测和抓取请求。"),
    "http_p99": ("会话列表 P99", "ms", '1000*histogram_quantile(0.99,sum by(le)(rate(nebula_http_handler_seconds_bucket{route="conversations"}[5m])))', "从业务 handler 开始到响应就绪。展开链路查看数据库调用。"),
    "history_p99": ("消息历史 P99", "ms", '1000*histogram_quantile(0.99,sum by(le)(rate(nebula_http_handler_seconds_bucket{route="history"}[5m])))', "仅消息历史业务 handler。"),
    "ws": ("WebSocket 连接", "连接", "webserver_websocket_online", "当前在线连接。"),
    "sse": ("SSE 连接", "连接", "webserver_sse_online", "当前在线连接。"),
    "queue": ("数据库任务排队", "项", "webserver_phase11_db_executor_queued", "结合队列等待时间和连接池使用量判断。"),
    "pool": ("连接池使用量", "连接", "webserver_phase11_postgres_pool_in_use", "借用中的连接数。"),
    "outbox": ("Outbox 投递速率", "条/秒", "rate(webserver_phase11_outbox_published_total[2m])", "与聊天持久化 ACK 分开观察。"),
    "cpu": ("主机 CPU 使用率", "%", '100*(1-avg(rate(node_cpu_seconds_total{mode="idle"}[2m])))', "主机总体使用率。"),
    "memory": ("主机内存使用率", "%", "100*(1-node_memory_MemAvailable_bytes/node_memory_MemTotal_bytes)", "以可用内存为准。"),
    "disk": ("根文件系统可用空间", "GiB", 'node_filesystem_avail_bytes{mountpoint="/",fstype!~"rootfs|overlay"}/1073741824', "查看监控保留空间。"),
    "device": ("设备窗口样本数", "帧", "nebula_observer_device_window_samples", "最近一个降采样窗口的帧数，按模拟来源固定映射。"),
}
for stage, title in zip(STAGES, ("任务排队", "取连接", "提交前累计", "COMMIT", "ACK 就绪累计")):
    PANELS["chat_" + stage] = (title + " P99", "ms",
        '1000*histogram_quantile(0.99,sum by(le)(rate(nebula_chat_stage_seconds_bucket{stage="' + stage + '"}[5m])))',
        "服务端处理阶段。累计值之间有重叠，分位数不能相减。")


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

    def trace(self, trace_id):
        return self.fetch(self.tempo, "/api/v2/traces/" + trace_id, trace=True)
