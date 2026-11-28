"""Bounded diagnostic sampling for an isolated NEBULA when Prometheus is absent."""
from collections import deque
from http.client import HTTPConnection, HTTPException
import json
import math
from pathlib import Path
import re
from secrets import token_hex
import shutil
import sqlite3
import threading
import time

from tools.observability.observer.metrics.histogram import query_histogram, quantile
from tools.observability.observer.metrics.service_levels import HTTP_TRACE_METRICS, TRACE_METRICS, capture_counters, report as service_level_report
from tools.observability.observer.sources.sources import (BackendError, BUSINESS_ALERTS, COUNTERS, DB_MAX_US, FUNCTIONS, GAUGES,
                      HTTP_RESPONSES, HTTP_ROUTES, PANELS, SIGNALS, STAGES)
from tools.observability.observer.storage.store import DENSE_METRIC_SECONDS, HISTORICAL_STEP
from tools.observability.observer.metrics.measurement_quality import empty_diagnostic
from tools.observability.observer.contracts.operation_catalog import FUNCTION_CALLS, FUNCTION_IN_FLIGHT

INTERVAL = 5
MAX_METRICS = 256 * 1024
FAMILIES = ("nebula_http_handler_seconds", "nebula_chat_stage_seconds", "nebula_function_seconds")
LINE = re.compile(r'^(\w+)(?:\{([^}]+)\})?\s+(\S+)$')
LABEL = re.compile(r'(\w+)="([^"]*)"')
QUERY_IDS = {panel[2]: key for key, panel in PANELS.items()}
SCRAPED_NAMES = frozenset((*COUNTERS.values(), *GAUGES.values(), DB_MAX_US, HTTP_RESPONSES,
                           *TRACE_METRICS, *HTTP_TRACE_METRICS, FUNCTION_CALLS, FUNCTION_IN_FLIGHT))


def parse_metrics(body):
    metrics = {}
    for line in body.decode("utf-8").splitlines():
        if not (match := LINE.fullmatch(line)):
            continue
        name, raw_labels, raw_value = match.groups()
        if name not in SCRAPED_NAMES and not name.startswith(FAMILIES):
            continue
        value = float(raw_value)
        if math.isfinite(value):
            metrics[name, tuple(sorted(LABEL.findall(raw_labels or "")))] = value
    return metrics


def total_difference(current, before):
    if current is not None and before is not None and current >= before:
        return current - before
    return None


def metric(metrics, name, **labels):
    return metrics.get((name, tuple(sorted(labels.items()))))


def histogram_points(current, before, elapsed, name, labels, prefix, percentiles, windows=None):
    def delta(suffix, **extra):
        return total_difference(metric(current, name + suffix, **labels, **extra),
                                metric(before, name + suffix, **labels, **extra))

    count, duration = delta("_count"), delta("_sum")
    if count is None:
        return {}
    values = {prefix + "_rate": count / elapsed}
    if count == 0:
        return values
    if duration is not None:
        values[prefix + "_avg"] = 1000 * duration / count
    limits = [(float(tags["le"]), tags["le"]) for family, keys in current if family == name + "_bucket"
              and (tags := dict(keys)) and "le" in tags and all(tags.get(k) == v for k, v in labels.items())]
    buckets = [(limit, delta("_bucket", le=raw)) for limit, raw in sorted(limits)]
    if (len(buckets) < 2 or buckets[-1] != (math.inf, count)
            or any(v is None for _, v in buckets) or any(a[1] > b[1] for a, b in zip(buckets, buckets[1:]))):
        return values
    if windows is not None:
        windows.setdefault(name,{})[next(iter(labels.values()))]=[
            ["+Inf" if math.isinf(bound) else bound, count] for bound,count in buckets]
    for p in percentiles:
        values[f"{prefix}_p{p}"] = quantile(p / 100,buckets) * 1000
    return values


def response_ratio(current,old):
    counts=[total_difference(metric(current,HTTP_RESPONSES,status_class=f"{i}xx"),
                             metric(old,HTTP_RESPONSES,status_class=f"{i}xx")) for i in range(1,6)]
    if any(value is None for value in counts) or not (total:=sum(counts)):
        return None
    return 100*counts[-1]/total


def business_alerts(values, active_since, stamp):
    rules = []
    readings = {"NebulaHttpHighErrors":values.get("http_5xx_pct"),
                "NebulaDatabaseMaxSlow":values["db_lifetime_max"]*1000 if "db_lifetime_max" in values else None}
    for key,rule in BUSINESS_ALERTS.items():
        current=readings[key]
        if current is None or current<=rule["threshold"]:
            active_since.pop(key,None)
            state="no_data" if current is None else "normal"
        else:
            started=active_since.setdefault(key,stamp)
            state="firing" if stamp-started>=rule["pending"] else "pending"
        started=active_since.get(key)
        rules.append({"labels":{"alertname":key},"state":state,"value":current,
                      "activeAt":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime(started)) if started else None,
                      "annotations":{**rule,"expression":rule["expr"]}})
    return rules


def operating_system():
    host, counters = {}, {}
    try:
        ticks = [int(n) for n in Path("/proc/stat").read_text().splitlines()[0].split()[1:9]]
        counters.update(total=sum(ticks), idle=ticks[3])
        memory = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
        host["memory"] = 100 * (1 - int(memory["MemAvailable"].split()[0]) / int(memory["MemTotal"].split()[0]))
        interfaces = [row.split(":", 1)[1].split() for row in Path("/proc/net/dev").read_text().splitlines()[2:]
                      if row.split(":", 1)[0].strip() != "lo"]
        counters.update(network_rx=sum(int(row[0]) for row in interfaces),
                        network_tx=sum(int(row[8]) for row in interfaces))
        disk = shutil.disk_usage("/")
        host["disk"] = disk.free / 1024**3
    except (OSError, ValueError, IndexError, KeyError, ZeroDivisionError):
        pass
    return host, counters


class LocalMetrics:
    profile = "local"
    probe = "local"

    def __init__(self, port, token, pid, threads_reader, store=None, scope_name="lab"):
        self.port, self.token, self.pid = port, token, pid
        self.threads_reader = threads_reader
        self.store = store
        self.scope_name = scope_name
        self.sampling_epoch = token_hex(8)
        self.raw = deque(maxlen=300 // INTERVAL + 2)
        self.rows = deque(maxlen=(DENSE_METRIC_SECONDS+HISTORICAL_STEP)//INTERVAL+1)
        self.recent_histograms = deque(maxlen=3600//INTERVAL)
        self.signals = {}
        self.alert_since = {}
        self.rule_since = {}
        self.rule_states = []
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.worker = threading.Thread(target=self.loop, name="observer-local-sampler")
        if store:
            with store.connect() as db:
                db.execute("CREATE TABLE IF NOT EXISTS metric_samples(stamp INTEGER PRIMARY KEY, values_json TEXT NOT NULL)")
                db.execute("CREATE TABLE IF NOT EXISTS histogram_windows(stamp INTEGER PRIMARY KEY, body TEXT NOT NULL)")
                rows = db.execute("SELECT * FROM metric_samples WHERE stamp>=? ORDER BY stamp",
                                  (self.dense_floor(),)).fetchall()
            self.rows.extend((row["stamp"],json.loads(row["values_json"])) for row in rows)

    def __enter__(self):
        self.worker.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.worker.join()

    def request(self, route, metrics=False):
        conn = HTTPConnection("127.0.0.1", self.port, timeout=1.25)
        try:
            conn.request("GET", route, headers={"Authorization": "Bearer " + self.token} if metrics and self.token else {})
            reply = conn.getresponse()
            limit = MAX_METRICS if metrics else 4096
            body = reply.read(limit + 1)
            return (reply.status, body) if len(body) <= limit else (0, b"")
        except (OSError, HTTPException):
            return 0, b""
        finally:
            conn.close()

    def fetch(self, _, path):
        status, body = self.request("/health" + path)
        if status != 200:
            raise BackendError("运行版本不可用")
        return json.loads(body)

    def trace(self, _):
        raise BackendError("轻量实时模式未连接 Tempo")

    def scope(self):
        return {"bc":self.scope_name,"app":"nebula","instance":f"127.0.0.1:{self.port}"}

    @staticmethod
    def dense_floor():
        return (int(time.time())-DENSE_METRIC_SECONDS)//HISTORICAL_STEP*HISTORICAL_STEP

    def retention_floor(self):
        return int(time.time())-self.store.retention_seconds if self.store else 0

    def range_step(self,start,step):
        return max(step,HISTORICAL_STEP) if self.store and start<self.dense_floor() else step

    def histogram_rows(self,start,end,step):
        if self.store:
            with self.store.read() as db:
                rows=db.execute("SELECT stamp,body FROM histogram_windows WHERE stamp IN ("
                                "SELECT max(stamp) FROM histogram_windows WHERE stamp BETWEEN ? AND ? "
                                "GROUP BY (stamp-?)/?) ORDER BY stamp",(start,end,start,step)).fetchall()
        else:
            with self.lock:
                rows=[(stamp,value) for stamp,value in self.recent_histograms if start<=stamp<=end]
        return [(stamp,json.loads(body)) for stamp,body in rows]

    def histogram(self,expr,start,end,step):
        start=max(start,self.retention_floor())
        step=self.range_step(start,step)
        return query_histogram(expr,self.histogram_rows(start,end,step),self.scope(),step)

    def instrumentation(self,start,end):
        rows=self.metric_rows(start,end)
        diagnostic=empty_diagnostic("function_conversations_p99",rows,end,self.retention_floor(),self.range_step(start,5))
        stamp,values=rows[-1] if rows else (None,{})
        captured=values.get("_metrics",{})
        families=captured.get("histograms",[])
        available="nebula_function_seconds" in families
        target={**self.scope(),"instance":values.get('_instance',self.scope()['instance'])}
        return {"status":"ready" if available else diagnostic["reason"],"target":target,
                "requiredMetric":"nebula_function_seconds","available":available,"sampleAt":stamp,
                "scrapeStatus":captured.get("status"),"observedHistogramFamilies":families,
                "observedFunctions":captured.get("functions"),"functionOutcomes":captured.get("functionOutcomes"),
                "guide":"已采到函数直方图；延迟只在所选时间窗有实际调用时显示。" if available else diagnostic["guide"]}

    def metric_rows(self,start,end):
        start=max(start,self.retention_floor())
        dense=self.dense_floor() if self.store else start
        recent_start=max(start,dense)
        with self.lock:
            recent=[(stamp,values) for stamp,values in self.rows if recent_start<=stamp<=end]
        if start>=dense:
            return recent
        # Recent samples are already decoded by the sampler. Read only the older prefix.
        with self.store.read() as db:
            rows=db.execute("SELECT stamp,values_json FROM metric_samples WHERE stamp BETWEEN ? AND ? ORDER BY stamp",
                            (start,min(end,dense-1))).fetchall()
        return [(stamp,json.loads(body)) for stamp,body in rows]+recent

    def sample(self):
        alive = int(self.request("/health/live")[0] == 200)
        ready = int(self.request("/health/ready")[0] == 200)
        code, body = self.request("/metrics", True)
        try:
            current = parse_metrics(body) if code == 200 else {}
        except (ValueError, UnicodeError):
            current = {}
        scraped = int(metric(current, COUNTERS["http"]) is not None)
        host, counters = operating_system()
        stamp, monotonic = int(time.time()), time.monotonic()
        if self.raw and monotonic - self.raw[-1][0] > INTERVAL * 2.5:
            self.raw.clear()
            self.rule_since.clear()
        self.raw.append((monotonic, current, counters))
        host_tick, _, old_host = next((point for point in reversed(self.raw) if monotonic - point[0] >= 120), self.raw[0])
        elapsed = monotonic - host_tick
        if elapsed > 0:
            total = total_difference(counters.get("total"), old_host.get("total"))
            idle = total_difference(counters.get("idle"), old_host.get("idle"))
            if total and idle is not None and idle <= total:
                host["cpu"] = 100 * (1 - idle / total)
            for key in ("network_tx", "network_rx"):
                if (amount := total_difference(counters.get(key), old_host.get(key))) is not None:
                    host[key] = amount / elapsed
        target=self.scope()["instance"]
        names={name for name,_ in current}
        values = {**host,"_instance":target,"_metrics":{"status":code,
                  "histograms":[family for family in FAMILIES if family+"_count" in names],
                  "functions":sorted({dict(labels).get('function') for family,labels in current
                                      if family=='nebula_function_seconds_count' and dict(labels).get('function') in FUNCTIONS.values()}),
                  "functionOutcomes":FUNCTION_CALLS in names}}
        windows={"instance":target}
        if (db_max:=metric(current,DB_MAX_US)) is not None:
            values["db_lifetime_max"]=db_max/1000
        if alive and (threads := self.threads_reader(self.pid)) is not None:
            values["threads"] = threads
        for key, name in GAUGES.items():
            if (value := metric(current, name)) is not None:
                values[key] = value
        for prefix,name in FUNCTIONS.items():
            if (value:=metric(current,FUNCTION_IN_FLIGHT,function=name)) is not None:
                values[prefix+'_in_flight']=value
        if not scraped:
            # Keep the observed gap, and prevent a later rate from bridging the outage.
            self.raw[-1] = (monotonic, {}, counters)
        contiguous = []
        for tick, raw, _ in reversed(self.raw):
            if not raw:
                break
            contiguous.append((tick, raw))
        if len(contiguous) >= 2:
            for key, name in COUNTERS.items():
                tick, raw = next((point for point in contiguous if monotonic - point[0] >= 120), contiguous[-1])
                if (amount := total_difference(metric(current, name), metric(raw, name))) is not None:
                    values[key] = amount / (monotonic - tick)
            tick, old = contiguous[-1]
            windows["seconds"]=round(monotonic-tick,2)
            if monotonic-tick>=295 and (rate:=response_ratio(current,old)) is not None:
                values["http_5xx_pct"]=rate
            for prefix, route in HTTP_ROUTES.items():
                values.update(histogram_points(current, old, monotonic - tick, FAMILIES[0], {"route": route},
                                               prefix, (50, 90, 95, 99),windows))
            for prefix,name in FUNCTIONS.items():
                values.update(histogram_points(current,old,monotonic-tick,FAMILIES[2],{"function":name},prefix,(99,),windows))
                amount=total_difference(metric(current,FUNCTION_CALLS,function=name,outcome='error'),
                                        metric(old,FUNCTION_CALLS,function=name,outcome='error'))
                if amount is not None:values[prefix+'_error_rate']=amount/(monotonic-tick)
            for stage in STAGES:
                prefix = "chat_" + stage
                points = histogram_points(current, old, monotonic - tick, FAMILIES[1], {"stage": stage}, prefix, (99,),windows)
                if prefix + "_p99" in points:
                    values[prefix] = points[prefix + "_p99"]
        lights = {"scrape": scraped, "alive": alive, "ready": ready, "monitor": 1}
        reliability = {"epoch": self.sampling_epoch, "live": bool(alive), "ready": bool(ready),
                       "scrape": bool(scraped), "persisted": False, "series": capture_counters(current) if scraped else {}}
        values["_reliability"] = reliability
        flags = {"NebulaUnreachable": not alive, "NebulaUnready": alive and not ready,
                 "NebulaScrapeFailed": not scraped, "NebulaQueueBacklog": values.get("queue", 0) > 0}
        windows_json=json.dumps(windows,separators=(",",":"))
        if self.store:
            try:
                reliability["persisted"] = True
                with self.store.connect() as db:
                    db.execute("INSERT OR REPLACE INTO metric_samples VALUES (?,?)", (stamp, json.dumps(values)))
                    db.execute("INSERT OR REPLACE INTO histogram_windows VALUES (?,?)",(stamp,windows_json))
            except sqlite3.OperationalError:
                # Expose degraded persistence while still publishing in-memory samples.
                reliability["persisted"] = False
                flags["NebulaHistoryWriteFailed"] = True
        with self.lock:
            self.rows.append((stamp, values))
            self.recent_histograms.append((stamp,windows_json))
            self.signals = {key: [stamp, value] for key, value in lights.items()}
            self.alert_since = {key: self.alert_since.get(key, stamp) for key, active in flags.items() if active}
            self.rule_states=business_alerts(values,self.rule_since,stamp)

    def loop(self):
        while not self.done.is_set():
            started = time.monotonic()
            self.sample()
            self.done.wait(max(0, INTERVAL - (time.monotonic() - started)))

    def service_levels(self, start, end):
        rows = self.metric_rows(min(start,end-DENSE_METRIC_SECONDS-INTERVAL),end)
        return service_level_report(rows, start, end, INTERVAL)

    def ranges(self,panels,start,end,step,diagnostics=None):
        floor=self.retention_floor()
        start=max(start,floor)
        step=self.range_step(start,step)
        buckets={}
        rows=[]
        if diagnostics is not None or any(not key.endswith("_p95") or key[:-4] not in FUNCTIONS for key in panels):
            rows=self.metric_rows(start,end)
            for stamp,values in rows:
                instance=values.get("_instance","历史隔离实例")
                buckets.setdefault(instance,{})[(stamp-start)//step]=(stamp,values)
        grouped=list(reversed(buckets.items()))
        histograms=None
        result={}
        for key in panels:
            if key.endswith("_p95") and (function:=FUNCTIONS.get(key[:-4])):
                if histograms is None:
                    histograms=self.histogram_rows(start,end,step)
                expr=('histogram_quantile(0.95, sum by (le) '
                      f'(rate(nebula_function_seconds_bucket{{function="{function}"}}[5m])))')
                data=query_histogram(expr,histograms,self.scope(),step,include_buckets=False)
                result[key]=[{"metric":row["labels"],"values":[
                    [stamp,"NaN" if value is None else str(value*1000)] for stamp,value in row["points"]]}
                    for row in data["series"]]
            else:
                result[key]=[{"metric":{**self.scope(),"instance":instance},
                              "values":[(stamp,str(values.get(key,"NaN"))) for stamp,values in group.values()]}
                             for instance,group in grouped if any(key in values for _,values in group.values())]
            if diagnostics is not None and not any(
                    value not in (None,"NaN") for line in result[key] for _,value in line["values"]):
                diagnostics[key]=empty_diagnostic(key,rows,end,floor,step)
        return step,result

    def prom(self, action, params=None):
        if action=="query_range":
            key=QUERY_IDS[params["query"]]
            diagnostics={}
            actual,result=self.ranges([key],params["start"],params["end"],params["step"],diagnostics)
            return {"result":result[key],"step":actual,"diagnostic":diagnostics.get(key)}
        with self.lock:
            if action == "query":
                key = next((k for k, query in SIGNALS.items() if query == params["query"]), None)
                point = self.signals.get(key)
                return {"result": [{"value": point}] if point else []}
            if action == "alerts":
                return {"alerts": [
                    {"labels": {"alertname": key}, "state": "firing" if time.time() - stamp >= 30 else "pending",
                     "activeAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(stamp)),
                     "annotations": {"window": "30s"}}
                    for key, stamp in self.alert_since.items()]+self.rule_states}
