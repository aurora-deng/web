"""Normalize measured cost without mixing instruction, heap and syscall units."""
from collections import defaultdict
import json
import re

SYMBOL=re.compile(r"^\((\d+)\)(?:\s+(.*))?$")
METRICS=("tb","tbk","gb","gbk","eb","ebk","mb","mbk")


def callgrind(path):
    events=[]
    symbols={}
    rows=defaultdict(lambda: defaultdict(int))
    current=None
    skip=False
    with path.open(encoding="utf-8",errors="replace") as report:
        for line in report:
            line=line.strip()
            if line.startswith("events:"):
                events=line.split()[1:]
            elif line.startswith(("fn=","cfn=")):
                name=line.split("=",1)[1]
                if match:=SYMBOL.fullmatch(name):
                    ident, label=match.groups()
                    if label:
                        symbols[ident]=label
                    name=symbols.get(ident,"symbol "+ident)
                if line.startswith("fn="):
                    current=name
            elif line.startswith("calls="):
                skip=True
            elif line and (line[0].isdigit() or line[0] in "+-*") and current:
                if skip:
                    skip=False
                    continue
                for name,value in zip(events,line.split()[1:]):
                    if value.isdecimal():
                        rows[current][name]+=int(value)
    def summarize(key,limit=40):
        return [{"symbol":name[:500],"value":value[key],"instructions":value.get("Ir",0)}
                for name,value in sorted(rows.items(),key=lambda pair:pair[1].get(key,0),reverse=True)[:limit] if value.get(key)]
    return {"instructions":summarize("Ir"),"syscallNs":summarize("sysTime"),"syscallCpuNs":summarize("sysCpuTime"),
            "totalInstructions":sum(v.get("Ir",0) for v in rows.values())}


def dhat(path):
    data=json.loads(path.read_text(encoding="utf-8"))
    frames=data["ftbl"]
    points=[]
    summary=defaultdict(int)
    for p in data["pps"]:
        if not p.get("tb"):
            continue
        row={k:p.get(k,0) for k in METRICS}
        row["stack"]=[frames[i][:400] for i in p["fs"][:12]]
        points.append(row)
        for key in METRICS:
            summary[key]+=row[key]
    return {"totals":dict(summary),"allocations":sorted(points,key=lambda row:row["tb"],reverse=True)[:40],
            "retained":sorted((p for p in points if p["eb"]),key=lambda row:row["eb"],reverse=True)[:40]}


def syscalls(path):
    result=[]
    for line in path.read_text(encoding="utf-8").splitlines():
        cols=line.split()
        if len(cols) not in (5,6) or not re.fullmatch(r"\d+\.\d+",cols[0]) or cols[-1]=="total":
            continue
        result.append({"name":cols[-1],"wallSeconds":float(cols[1]),"usecPerCall":int(cols[2]),
                       "calls":int(cols[3]),"errors":int(cols[4]) if len(cols)==6 else 0})
    return sorted(result,key=lambda row:row["wallSeconds"],reverse=True)[:80]
