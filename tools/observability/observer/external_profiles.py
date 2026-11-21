"""Protocol adapters for existing profile and trace deployments."""
import base64
import json
import re
import time

from .events import Invalid
from .sources import BackendError

PYRO = "/querier.v1.QuerierService/"


def json_object(body):
    try:
        value=json.loads(body)
        if isinstance(value,dict):
            return value
    except (ValueError,TypeError):
        pass
    raise BackendError("数据源未返回 JSON 对象")


def profile_catalog(http,base,start,end):
    code,body=http.get(base,PYRO+"ProfileTypes",payload={"start":start*1000,"end":end*1000})
    if code!=200:
        raise BackendError(f"Pyroscope 返回 HTTP {code}")
    value=json_object(body).get("profileTypes",[])
    return [{"id":item.get("ID",item.get("id")),"unit":item.get("sampleUnit","")} for item in value[:60]
            if isinstance(item,dict) and isinstance(item.get("ID",item.get("id")),str)]


def query_profile(http,base,request,start,end):
    if request.get("action")=="catalog":
        return {"types":profile_catalog(http,base,start,end)}
    profile=request.get("profileType","")
    selector=request.get("selector","")
    if (not isinstance(profile,str) or not re.fullmatch(r"[a-zA-Z0-9_.:-]{1,200}",profile)
            or not isinstance(selector,str) or not selector.startswith("{") or not selector.endswith("}") or len(selector)>640):
        raise Invalid("需要 Profile Type ID 和标签选择器")
    body={"start":start*1000,"end":end*1000,"profileTypeID":profile,
          "labelSelector":selector,"maxNodes":400}
    code,raw=http.get(base,PYRO+"SelectMergeStacktraces",payload=body)
    if code!=200:
        raise BackendError(f"Pyroscope 返回 HTTP {code}")
    result=json_object(raw)
    graph=result.get("flamegraph",{})
    names=graph.get("names",[])
    if not isinstance(names,list) or not isinstance(graph.get("levels",[]),list):
        raise BackendError("Pyroscope 火焰图格式无效")
    levels=[]
    for row in graph.get("levels",[])[:120]:
        label=row.get("values",[]) if isinstance(row,dict) else row
        x=0
        items=[]
        for i in range(0,len(label)-3,4):
            try:
                gap,total,self_cost,which=map(int,label[i:i+4])
                if min(gap,total,self_cost,which)<0 or which>=len(names):
                    raise ValueError()
            except (ValueError,TypeError):
                raise BackendError("Pyroscope 火焰图行格式无效") from None
            x+=gap
            items.append({"x":x,"total":total,"self":self_cost,"name":str(names[which])[:300]})
            x+=total
        levels.append(items[:400])
    return {"profileType":profile,"unit":profile.split(":")[2] if profile.count(":")>=2 else "",
            "total":int(graph.get("total",0)),"levels":levels}


def hex_id(value):
    if not isinstance(value,str) or len(value)>32:
        return ""
    if re.fullmatch(r"(?:[0-9a-f]{16}|[0-9a-f]{32})",value):
        return value
    try:
        return base64.b64decode(value,validate=True).hex()
    except ValueError:
        return ""


def flatten_jaeger(payload):
    """Jaeger v3 HTTP streams protobuf-JSON trace chunks as newline-separated results."""
    root=[]
    for line in payload.decode("utf-8").splitlines():
        if not line.strip():
            continue
        data=json_object(line)
        result=data.get("result",data)
        trace=result.get("trace",result)
        batches=trace.get("resourceSpans",trace.get("batches",[]))
        for batch in batches:
            for scope in batch.get("scopeSpans",[]):
                for s in scope.get("spans",[]):
                    for key in ("traceId","spanId","parentSpanId"):
                        if key in s:
                            s[key]=hex_id(s[key])
            root.append(batch)
    return {"resourceSpans":root}


def jaeger(http,base,request,start,end):
    trace=request.get("trace","")
    if trace:
        code,body=http.get(base,"/api/v3/traces/"+trace)
        if code==400:
            from urllib.parse import quote
            code,body=http.get(base,"/api/v3/traces/"+quote(base64.b64encode(bytes.fromhex(trace)).decode("ascii"),safe=""))
        if code!=200:
            raise BackendError(f"Jaeger 返回 HTTP {code}")
        return flatten_jaeger(body)
    service=request.get("expr","").strip()
    if not re.fullmatch(r"[\w./-]{1,128}",service):
        raise Invalid("Jaeger 搜索需填写精确的服务名称")
    utc=lambda value:time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime(value))
    params={"query.serviceName":service,"query.startTimeMin":utc(start),
            "query.startTimeMax":utc(end),"query.searchDepth":30}
    code,body=http.get(base,"/api/v3/traces",params)
    if code!=200:
        raise BackendError(f"Jaeger 返回 HTTP {code}")
    grouped={}
    for b in flatten_jaeger(body)["resourceSpans"]:
        service_tag=next((tag["value"].get("stringValue") for tag in b.get("resource",{}).get("attributes",[])
                          if tag.get("key")=="service.name"),service)
        for scope in b.get("scopeSpans",[]):
            for span in scope.get("spans",[]):
                ident=span.get("traceId","")
                if len(ident)!=32:
                    continue
                started,finished=int(span["startTimeUnixNano"]),int(span["endTimeUnixNano"])
                saved=grouped.setdefault(ident,{"traceID":ident,"rootServiceName":service_tag,
                    "rootTraceName":span.get("name",""),"durationMs":0,"start":started,"end":finished})
                saved.update(start=min(saved["start"],started),end=max(saved["end"],finished))
                if not span.get("parentSpanId"):
                    saved.update(rootServiceName=service_tag,rootTraceName=span.get("name",""))
    return {"traces":[{**item,"durationMs":round((item["end"]-item["start"])/1e6,3)}
                      for item in list(grouped.values())[:30]]}
