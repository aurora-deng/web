"""Open the same Observer UI with explicitly labelled synthetic telemetry."""
import argparse
import math
from pathlib import Path
import time

from .observer.server import ObserverServer
from .observer.sources import NoTrace, PANELS
from .observer.store import Store
from .scenarios import agent, dag, device


class PreviewSources:
    def __init__(self, traces):
        self.traces = traces
        self.probe = "preview"

    def fetch(self, *_):
        return {"revision": "模拟预览"}

    def trace(self, trace_id):
        if trace_id not in self.traces:
            raise NoTrace
        return self.traces[trace_id]

    def prom(self, route, args=None):
        now = int(time.time())
        if route == "alerts":
            return {"alerts": [{"labels": {"alertname":"NebulaQueueBacklog"},
                "state":"firing", "activeAt":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime(now-240)),
                "annotations":{"guide":"模拟演练。查看同一时段的任务排队和连接池，比较是否一起上涨。"}}]}
        if route == "query":
            return {"result":[{"value":[now, "1"]}]}
        start, end = int(args["start"]), int(args["end"])
        step = max(1, int(args["step"]))
        by_query = {value[2]:key for key,value in PANELS.items()}
        panel = by_query[args["query"]]
        levels = {"http":470,"memory":23,"cpu":34,"disk":6.9,"ws":16,"sse":8,"queue":7,"pool":8,
                  "outbox":135,"chat_queue":20,"chat_pool":3,"chat_before_commit":15,"chat_commit":3,"chat_ack_ready":18,
                  "threads":18, "network_tx":410 * 1024, "network_rx":240 * 1024,
                  "http_rate":82, "history_rate":43, "auth_rejected":.3, "rate_limited":.08,
                  "db_failed":.03, "origin_rejected":0, "outbox_failed":0}
        for prefix, factor in (("http", 1), ("history", 1.7)):
            for kind, baseline in (("avg",7),("p50",5),("p90",12),("p95",17),("p99",24)):
                levels[prefix + "_" + kind] = baseline * factor
        level = levels.get(panel, 12)
        phase = sum(map(ord, panel)) % 23 / 5
        values = []
        for stamp in range(start, end + 1, step):
            t = (stamp - start) / (end - start)
            if .34 < t < .41 and panel.endswith(("avg", "p50", "p90", "p95", "p99")):
                values.append([stamp, "NaN"])
                continue
            wave = 0.83 + .11 * math.sin(t * 17 + phase) + .05 * math.sin(t * 170 + phase)
            value = level * (wave + .43 * math.exp(-((t - .72) / .078) ** 2))
            if panel in ("threads", "ws", "sse"):
                value = round(value)
            values.append([stamp,str(round(value,3))])
        return {"result":[{"metric":{"instance":"模拟实例"},"values":values}]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir",type=Path,required=True,help="Choose a new directory for the simulated event database")
    parser.add_argument("--port",type=int,default=18785)
    args=parser.parse_args()
    args.data_dir.mkdir(parents=True,exist_ok=False)
    store=Store(args.data_dir/"preview.sqlite")
    traces={}
    for domain, builder in (("dag",dag),("agent",agent),("device",device)):
        scenario=builder()
        store.append(domain+"-preview",domain,True,scenario.events)
        if scenario.spans:
            traces[scenario.trace]={"trace":{"resourceSpans":[{"resource":{"attributes":[
                {"key":"service.name","value":{"stringValue":domain+"-preview"}}]},
                "scopeSpans":[{"spans":scenario.spans}]}]}}
    with ObserverServer(("127.0.0.1",args.port),store,PreviewSources(traces),[],preview=True) as server:
        print("模拟预览：http://127.0.0.1:"+str(server.server_port),flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__=="__main__":
    main()
