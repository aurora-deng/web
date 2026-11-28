"""Read the locally stored classic histogram windows for the Explore evaluator."""
from bisect import bisect_left
from collections import defaultdict
import json
import math
import re

from tools.observability.observer.contracts.events import Invalid

FAMILY_LABELS = {
    "nebula_http_handler_seconds": "route",
    "nebula_chat_stage_seconds": "stage",
    "nebula_function_seconds": "function",
}
EXPRESSION = re.compile(
    r'histogram_quantile\(\s*(0(?:\.\d+)?|1(?:\.0+)?)\s*,\s*'
    r'sum\s+by\s*\(\s*le(?:\s*,\s*(route|stage|function))?\s*\)\s*'
    r'\(\s*rate\(\s*(\w+)_bucket\s*(?:\{\s*(\w+)\s*=\s*"([A-Za-z0-9_:.\-]+)"\s*\})?'
    r'\[5m\]\s*\)\s*\)\s*\)'
)


def quantile(q, buckets):
    total = buckets[-1][1]
    if not total:
        return None
    rank = q * total
    index = bisect_left([count for _,count in buckets[:-1]],rank)
    upper, upto = buckets[index]
    lower, previous = buckets[index-1] if index else (0,0)
    if math.isinf(upper) or upto == previous:
        return lower
    return lower + (upper-lower) * (rank-previous) / (upto-previous)


def query_histogram(expr, rows, labels, step, include_buckets=True):
    match = EXPRESSION.fullmatch(expr.strip())
    if not match:
        raise Invalid("轻量档支持 histogram_quantile(q, sum by (le) (rate(延迟桶[5m])))；完整 PromQL 请切换 Prometheus")
    q, group, family, key, selected = match.groups()
    label = FAMILY_LABELS.get(family)
    if not label or any(item and item!=label for item in (group,key)):
        raise Invalid("延迟桶名称或分组标签未登记")
    series = {}
    histograms = {}
    stamps = []
    for stamp, body in rows:
        windows = json.loads(body) if isinstance(body,str) else body
        instance=windows.get("instance",labels["instance"])
        counts = defaultdict(lambda: defaultdict(float))
        for value,buckets in windows.get(family,{}).items():
            if selected and value!=selected:
                continue
            target = counts[value if group else selected or ""]
            for edge,count in buckets:
                target[float(edge)] += count
        stamps.append(stamp)
        for name,bucket in counts.items():
            cumulative = sorted(bucket.items())
            estimate = quantile(float(q),cumulative)
            series.setdefault((instance,name),{})[stamp] = estimate
            if estimate is not None:
                histograms[instance,name] = (stamp,windows["seconds"],cumulative)
    results=[]
    for (instance,name),points in series.items():
        if (instance,name) not in histograms:
            continue
        at,seconds,cumulative=histograms[instance,name]
        row={"labels":{**labels,"instance":instance,**({label:name} if name else {})},
             "points":[[stamp,points.get(stamp)] for stamp in stamps]}
        if include_buckets:
            previous=0
            bars=[]
            for edge,count in cumulative:
                bars.append(["+Inf" if math.isinf(edge) else edge,int(count-previous)])
                previous=count
            row["histogram"]={"at":at,"sampleSeconds":seconds,"count":int(previous),"buckets":bars}
        results.append((at,row))
    results.sort(key=lambda item:item[0],reverse=True)
    return {"series":[row for _,row in results[:8]],"step":step,"unit":"s",
            "status":"ok" if results else "empty","truncated":len(results)>8}
