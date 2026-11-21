"""Poll saved services and run bounded panel queries."""
from concurrent.futures import ThreadPoolExecutor
import json
import math
import re
import sqlite3
import threading
import time

from .events import Invalid
from .sources import BackendError, PANELS
from .store import DENSE_METRIC_SECONDS, HISTORICAL_STEP, encode
from .target_http import TargetHttp
from .workspace import BUILTIN, source_body
from .external_profiles import jaeger, profile_catalog, query_profile

INTERVAL = 15
SAMPLE_LIMIT = 48
RESOURCE_LABELS = ("bc","app","cluster","namespace","service","job","pod","instance","node")
LINE = re.compile(r'^([A-Za-z_:]\w*(?::\w+)*)\s*(\{(?:[^"\\\n]|"(?:[^"\\\n]|\\.)*")*\})?\s+(\S+)(?:\s+-?\d+)?$')
LABEL = re.compile(r'([A-Za-z_]\w*)\s*=\s*"((?:[^"\\]|\\.)*)"')


def matrix_rows(data,kind):
    if kind=='instant' and isinstance(data,dict) and data.get('resultType')=='vector':
        data={'resultType':'matrix','result':[{'metric':row['metric'],'values':[row['value']]} for row in data['result']]}
    elif kind=='instant' and isinstance(data,dict) and data.get('resultType')=='scalar':
        data={'resultType':'matrix','result':[{'metric':{},'values':[data['result']]}]}
    if not isinstance(data,dict) or data.get('resultType')!='matrix':
        raise BackendError('Prometheus 未返回时间序列')
    return data.get('result')


def samples(body, wanted=()):
    text = body.decode("utf-8", errors="replace")
    declared = any(line.startswith(("# HELP ", "# TYPE ", "# UNIT ", "# EOF")) for line in text.splitlines())
    result = {}
    for line in text.splitlines():
        match = LINE.fullmatch(line) if line and not line.startswith("#") else None
        if not match:
            continue
        name, raw_labels, value = match.groups()
        declared = True
        if wanted and name not in wanted:
            continue
        try:
            numeric = float(value)
        except ValueError:
            continue
        if not math.isfinite(numeric):
            numeric = None
        labels = dict(LABEL.findall(raw_labels or ""))
        # Retain bounded labels only, and restrict cardinality per sample.
        if len(labels)>10 or any(len(v)>160 for v in labels.values()):
            continue
        variants = result.setdefault(name,{})
        ident = encode(labels)
        if len(variants)<4 or ident in variants:
            variants[ident] = numeric
        if sum(map(len,result.values())) >= SAMPLE_LIMIT:
            break
    if not declared:
        raise Invalid("响应不是 Prometheus 文本指标")
    return result


def range_result(rows):
    if not isinstance(rows,list):
        raise BackendError("数据源返回了无效序列")
    result = []
    for series in rows[:8]:
        if not isinstance(series,dict):
            raise BackendError("数据源返回了无效序列")
        labels = series.get("metric",{})
        values = series.get("values",[])
        if not isinstance(labels,dict) or not isinstance(values,list):
            raise BackendError("数据源返回了无效采样")
        try:
            points = [[int(float(t)),n if math.isfinite(n:=float(value)) else None] for t,value in values[:721]]
        except (ValueError,TypeError,OverflowError) as error:
            raise BackendError("数据源返回了无效采样") from error
        result.append({"labels":{str(k)[:160]:str(v)[:160] for k,v in list(labels.items())[:10]},"points":points})
    return result


class SourceMonitor:
    def __init__(self, workspace, builtin, networks, port, token=None, auth=None):
        self.workspace, self.builtin = workspace,builtin
        self.http = TargetHttp(networks,port,auth)
        self.token = token
        self.lock = threading.Lock()
        self.results = {}
        self.done = threading.Event()
        self.worker = threading.Thread(target=self.loop,name="observer-target-poll",daemon=True)

    def start(self):
        self.worker.start()

    def close(self):
        self.done.set()
        if self.worker.is_alive():
            self.worker.join(timeout=10)

    def credential(self,url):
        # Existing lab token belongs only to this exact metrics endpoint.
        target, token = self.token or (None,None)
        return token if target==url else None

    def check(self,row):
        return self.inspect(source_body(row))[0]

    def inspect(self,source):
        kind,url = source["type"],source["url"]
        since = time.monotonic()
        if kind=="prometheus":
            data=self.http.prom(url,"query",{"query":"vector(1)"})
            if data.get("resultType")!="vector" or not data.get("result"):
                raise Invalid("地址未通过 Prometheus 查询校验")
            result={"ok":True,"code":200,"metrics":[]}
            readings={}
        elif kind in ("tempo","loki","jaeger"):
            path={"tempo":"/ready","loki":"/loki/api/v1/labels","jaeger":"/api/v3/services"}[kind]
            code,_=self.http.get(url,path,read_body=False)
            if code!=200:
                raise BackendError(f"{kind} 返回 HTTP {code}")
            result,readings={"ok":True,"code":code,"metrics":[]},{}
        elif kind=="pyroscope":
            now=int(time.time())
            types=profile_catalog(self.http,url,now-3600,now)
            result,readings={"ok":True,"code":200,"metrics":[p["id"] for p in types]},{}
        elif kind=="metrics":
            code,body=self.http.get(url,token=self.credential(url))
            if code!=200:
                raise BackendError(f"指标地址返回 HTTP {code}")
            reading=samples(body,source["metrics"])
            # Avoid persisting arbitrary labels and series unless explicitly selected.
            readings=reading if source["metrics"] else {}
            result={"ok":True,"code":code,"metrics":list(reading)[:24],
                    "missing":[name for name in source["metrics"] if name not in reading]}
        else:
            code,_=self.http.get(url,read_body=False)
            elapsed=round((time.monotonic()-since)*1000,3)
            readings={name:{"{}":value} for name,value in
                      (("probe_success",int(200<=code<300)),("probe_duration_ms",elapsed),("probe_http_status",code))}
            result={"ok":200<=code<300,"code":code,"metrics":list(readings)}
        return {**result,"durationMs":round((time.monotonic()-since)*1000,3)},readings

    def current(self,ident):
        with self.lock:
            return self.results.get(ident)

    def collector(self, source):
        stamp=int(time.time())
        try:
            report,values=self.inspect(source)
        except (Invalid,BackendError) as error:
            report,values={"ok":False,"message":str(error)},{}
            if source["type"]=="http":
                values={"probe_success":{"{}":0}}
        ident=source["id"]
        try:
            with self.workspace.store.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                row=db.execute("SELECT revision FROM observer_configs WHERE kind='source' AND id=?",(ident,)).fetchone()
                if not row or row["revision"]!=source["revision"]:
                    return
                db.execute("INSERT OR REPLACE INTO source_samples VALUES (?,?,?)",(ident,stamp,encode(values)))
        except sqlite3.OperationalError:
            report={"ok":False,"message":"采集历史暂不可写"}
        with self.lock:
            self.results[ident]={**report,"ts":stamp,"revision":source["revision"]}

    def loop(self):
        with ThreadPoolExecutor(max_workers=4) as jobs:
            while not self.done.is_set():
                start=time.monotonic()
                try:
                    tasks=[jobs.submit(self.collector,source) for source in self.workspace.list("source") if source["type"] in ("metrics","http")]
                    for task in tasks:
                        task.result()
                except sqlite3.OperationalError:
                    pass
                self.done.wait(max(0,INTERVAL-(time.monotonic()-start)))

    def historical(self,source,expr,start,end,step):
        if source["type"]=="metrics" and expr not in source["metrics"]:
            raise Invalid("先在数据源中将这个指标加入采集列表")
        found={}
        buckets={}
        start=max(start,int(time.time())-self.workspace.store.retention_seconds)
        with self.workspace.store.connect() as db:
            rows=db.execute("SELECT ts,body FROM source_samples WHERE source=? AND ts IN ("
                            "SELECT max(ts) FROM source_samples WHERE source=? AND ts BETWEEN ? AND ? "
                            "GROUP BY (ts-?)/?) ORDER BY ts",
                            (source["id"],source["id"],start,end,start,step))
            for row in rows:
                buckets[(row["ts"]-start)//step]=(row["ts"],json.loads(row["body"]).get(expr,{}))
        for _,values in buckets.values():
            for labels in values:
                found.setdefault(labels,json.loads(labels))
        return [{"metric":labels,"values":[[stamp,values.get(key)] for stamp,values in buckets.values()]}
                for key,labels in list(found.items())[:8]]

    def query(self,request,start,end):
        expr=request.get("expr")
        if not isinstance(expr,str) or not 1<=len(expr)<=1024:
            raise Invalid("查询长度需介于 1 到 1024 字符")
        source=request.get("source")
        started=time.monotonic()
        kind=request.get("queryType","range")
        configured=request.get("stepSeconds")
        if kind not in ("range","instant") or configured is not None and (type(configured) is not int or not 5<=configured<=3600):
            raise Invalid("查询类型为 range/instant，指定步长为 5 至 3600 秒")
        minimum=5 if source==BUILTIN and getattr(self.builtin,"profile",None)=="local" else 15
        step=max(configured or minimum,math.ceil((end-start)/600))
        def finish(response):
            if kind=="instant":
                for row in response["series"]:
                    row["points"]=[point for point in row["points"] if point[1] is not None][-1:]
            return {**response,"queryType":kind,"evaluatedAt":end}
        if source==BUILTIN:
            if request.get("queryMode")=="promql":
                if hasattr(self.builtin,'query_promql'):
                    rows=matrix_rows(self.builtin.query_promql(expr,start,end,step,kind),kind)
                    return finish(self.response(range_result(rows),step,started,len(rows)>8))
                if not hasattr(self.builtin,"histogram"):
                    raise Invalid("请选择已配置的 Prometheus 数据源查询自由表达式")
                response=self.builtin.histogram(expr,start,end,step)
                if response["status"]=="empty" and hasattr(self.builtin,"instrumentation"):
                    response["diagnostic"]=self.builtin.instrumentation(start,end)
                return finish({**response,"queryMs":round((time.monotonic()-started)*1000,2)})
            if expr not in PANELS:
                raise Invalid("选择一个内置指标")
            result=self.builtin.prom("query_range",{"query":PANELS[expr][2],"start":start,"end":end,"step":step})
            rows=result["result"]
            step=result.get("step",step)
        else:
            config=self.workspace.get("source",source)
            if config["type"]=="prometheus":
                scope=request.get("scope",{})
                if not isinstance(scope,dict) or any(k not in RESOURCE_LABELS or not isinstance(v,str) or len(v)>150
                                                       for k,v in scope.items()):
                    raise Invalid("资源筛选无效")
                def expand(match):
                    value=scope.get(match[1],"")
                    if not value:
                        return ".*"
                    escaped=re.sub(r'([.\\+*?\[\](){}^$|])',r'\\\1',value)
                    return json.dumps(escaped,ensure_ascii=False)[1:-1]
                expr=re.sub(r'\$\{('+"|".join(RESOURCE_LABELS)+r')\}',expand,expr)
                data=self.http.prom(config["url"],"query" if kind=="instant" else "query_range",
                    {"query":expr,"time":end} if kind=="instant" else {"query":expr,"start":start,"end":end,"step":step})
                rows=matrix_rows(data,kind)
            elif config["type"] in ("metrics","http"):
                return finish(self.collected(config,expr,start,end,step,started))
            else:
                raise Invalid("链路和日志数据源请在分布式排障页查询")
        result=range_result(rows)
        return finish(self.response(result,step,started,len(rows)>8))

    def labels(self,request,start,end):
        src=self.workspace.get("source",request.get("source"))
        label=request.get("label")
        if src["type"]!="prometheus" or label not in RESOURCE_LABELS:
            raise Invalid("请选择 Prometheus 数据源和标准资源标签")
        params={"start":start,"end":end,"limit":100}
        scope=request.get("scope",{})
        parents={"app":("bc",),"instance":("bc","app")}.get(label,())
        if not isinstance(scope,dict) or any(k not in parents or not isinstance(v,str) or len(v)>150 for k,v in scope.items()):
            raise Invalid("上级标签筛选无效")
        if scope:
            params["match[]"]="{"+",".join(key+"="+json.dumps(val,ensure_ascii=False) for key,val in scope.items() if val)+"}"
            if params["match[]"]=="{}":
                params.pop("match[]")
        data=self.http.prom(src["url"],"label/"+label+"/values",params)
        if label=="service" and data==[]:
            data=self.http.prom(src["url"],"label/service_name/values",params)
        if not isinstance(data,list):
            raise BackendError("Prometheus 标签查询返回格式异常")
        return {"values":[v for v in data[:100] if isinstance(v,str) and len(v)<=150]}

    def investigate(self, request, start, end):
        src=self.workspace.get("source",request.get("source"))
        query=request.get("expr","")
        trace=request.get("trace","")
        if not isinstance(query,str) or len(query)>1024:
            raise Invalid("查询超过长度限制")
        if not isinstance(trace,str) or trace and not re.fullmatch("[a-f0-9]{32}",trace):
            raise Invalid("Trace ID 应为 32 位十六进制字符")
        base=src["url"]
        if src["type"]=="tempo":
            if trace:
                return self.read_json(base,"/api/v2/traces/"+trace)
            return self.read_json(base,"/api/search",{"start":start,"end":end,"limit":30,"q":query or "{}"})
        if src["type"]=="jaeger":
            return jaeger(self.http,base,request,start,end)
        if src["type"]!="loki" or not query.strip():
            raise Invalid("日志查询需要 Loki 数据源和 LogQL")
        if trace:
            query+=" |= "+json.dumps(trace)
        payload=self.read_json(base,"/loki/api/v1/query_range",
                               {"query":query,"start":start*10**9,"end":end*10**9,"limit":200,"direction":"backward"})
        data=payload.get("data",{})
        if payload.get("status")!="success" or data.get("resultType")!="streams":
            raise BackendError("Loki 未返回日志，请使用日志流选择器")
        result=[]
        for stream in data["result"][:24]:
            labels={k:str(v)[:160] for k,v in list(stream["stream"].items())[:12]}
            for stamp,line in stream["values"][:200]:
                result.append({"time":stamp,"line":line[:2000],"labels":labels})
        return {"logs":sorted(result,key=lambda row:int(row["time"]),reverse=True)[:200]}

    def profile(self,request,start,end):
        src=self.workspace.get("source",request.get("source"))
        if src["type"]!="pyroscope":
            raise Invalid("请选择 Pyroscope 数据源")
        return query_profile(self.http,src["url"],request,start,end)

    def read_json(self,base,suffix,params=None):
        code,body=self.http.get(base,suffix,params)
        if code!=200:
            raise BackendError(f"数据源查询返回 HTTP {code}")
        try:
            data=json.loads(body)
            if isinstance(data,dict):
                return data
        except ValueError:
            pass
        raise BackendError("数据源未返回有效 JSON")

    def collected(self,config,expr,start,end,step,started):
        dense=(int(time.time())-DENSE_METRIC_SECONDS)//HISTORICAL_STEP*HISTORICAL_STEP
        if start<dense:
            step=max(step,HISTORICAL_STEP)
        rows=self.historical(config,expr,start,end,step)
        result=[{"labels":row["metric"],"points":row["values"]} for row in rows]
        return self.response(result,step,started)

    @staticmethod
    def response(series,step,started,truncated=False):
        return {"series":series,"status":"ok" if any(v is not None for row in series for _,v in row["points"]) else "empty",
                "queryMs":round((time.monotonic()-started)*1000,2),"step":step,"truncated":truncated}
