"""Open the same Observer UI with explicitly labelled synthetic telemetry."""
import argparse
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
                  "outbox":135,"chat_queue":20,"chat_pool":3,"chat_before_commit":15,"chat_commit":3,"chat_ack_ready":18}
        level = levels.get(panel, 12)
        values = [[stamp,str(round(level * (0.65+(index*13%16)/40 + (0.35 if 0.6 < (stamp-start)/(end-start) < 0.8 else 0)),3))]
                  for index,stamp in enumerate(range(start,end+1,step))]
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
