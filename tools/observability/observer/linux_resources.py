"""Read-only Linux resource counters. A missing counter never becomes a zero."""
from collections import deque
import os
from pathlib import Path
import socket
import time

TICKS = getattr(os,"sysconf",lambda _ : 100)("SC_CLK_TCK")
PER_CPU = ("user","nice","system","idle","iowait","irq","softirq","steal")
LIMIT_PROCESSES = 32
LIMIT_THREADS = 48


def read(path):
    try:
        return path.read_text(encoding="utf-8",errors="replace")
    except OSError:
        return ""


def ints(lines, separator=None):
    values = {}
    for line in lines.splitlines():
        parts=line.split(separator,1) if separator else line.split(None,1)
        if len(parts)!=2:
            continue
        try:
            values[parts[0]]=int(parts[1].split()[0])
        except (ValueError,IndexError):
            pass
    return values


def cpu_stat(text):
    cpus={}
    for line in text.splitlines():
        fields=line.split()
        if fields and (fields[0]=="cpu" or fields[0].startswith("cpu") and fields[0][3:].isdigit()):
            cpus[fields[0]]=dict(zip(PER_CPU,map(int,fields[1:9])))
    return cpus


def paired(text):
    lines=text.splitlines()
    values={}
    for label,row in zip(lines[::2],lines[1::2]):
        fields,numbers=label.split(),row.split()
        if not fields or not numbers or fields[0]!=numbers[0]:
            continue
        for name,value in zip(fields[1:],numbers[1:]):
            if value.lstrip("-").isdecimal():
                values[fields[0]+name]=int(value)
    return values


def process_stat(text):
    if ")" not in text or "(" not in text:
        return None
    begin=text.index("(")
    end=text.rfind(")")
    a=text[end+2:].split()
    if len(a)<22:
        return None
    return {"name":text[begin+1:end][:80],"state":a[0],"parent":int(a[1]),"minor":int(a[7]),"major":int(a[9]),
            "userTicks":int(a[11]),"systemTicks":int(a[12]),"threads":int(a[17]),"start":int(a[19]),
            "lastCpu":int(a[36]) if len(a)>36 else None}


def delta(now,before,key):
    a,b=now.get(key),before.get(key)
    return a-b if a is not None and b is not None and a>=b else None


def quantize(value):
    return round(value,4) if value is not None else None


def proportions(now,before):
    increments={key:delta(now,before,key) for key in PER_CPU}
    total=sum(value or 0 for value in increments.values())
    return {key:quantize(value*100/total) if total and value is not None else None for key,value in increments.items()}, total


def inspect_process(path, ident, service=None, detail=False, sched_delay=False, memtotal=None):
    state=process_stat(read(path/"stat"))
    if not state:
        return None
    options=read(path/"status")
    vals=ints(options,":")
    if not options or "Threads" not in vals:
        return None
    key=f"{ident}:{state['start']}"
    mask=next((line.partition(":")[2].strip() for line in options.splitlines() if line.startswith("Cpus_allowed_list:")),"")
    scheduling=read(path/"schedstat").split()
    process={**state,"pid":ident,"key":key,"service":service,"affinity":mask,
             "rssKiB":vals.get("VmRSS"), "swapKiB":vals.get("VmSwap"), "rssPct":(100*vals["VmRSS"]/memtotal if memtotal and "VmRSS" in vals else None),
             "voluntary":vals.get("voluntary_ctxt_switches"),"involuntary":vals.get("nonvoluntary_ctxt_switches")}
    if len(scheduling)==3:
        process["runtimeNs"]=int(scheduling[0])
        process["waitNs"]=int(scheduling[1]) if sched_delay else None
    if detail:
        process["pssKiB"]=ints(read(path/"smaps_rollup"),":").get("Pss")
        try:
            process["fds"]=sum(1 for _ in (path/"fd").iterdir())
        except OSError:
            process["fds"]=None
        io=ints(read(path/"io"),":")
        process["readBytes"]=io.get("read_bytes")
        process["writeBytes"]=io.get("write_bytes")
    return process


def process_rates(now, old, elapsed, full_ticks=0, busy_ticks=0):
    rates={}
    if not old or elapsed<=0:
        return rates
    user=delta(now,old,"userTicks")
    system=delta(now,old,"systemTicks")
    if user is not None and system is not None:
        ticks=user+system
        rates.update(cpuPct=quantize(100*ticks/TICKS/elapsed),userPct=quantize(100*user/TICKS/elapsed),
                     systemPct=quantize(100*system/TICKS/elapsed),
                     capacityPct=quantize(100*ticks/full_ticks) if full_ticks else None,
                     busyPct=quantize(100*ticks/busy_ticks) if busy_ticks else None)
    for key in ("minor","major","voluntary","involuntary","readBytes","writeBytes","runtimeNs","waitNs"):
        diff=delta(now,old,key)
        if diff is not None:
            rates[key+"PerSec"]=quantize(diff/elapsed/(1e6 if key.endswith("Ns") else 1))
    return rates


def snapshot(targets, proc=Path("/proc"), details=True):
    source=read(proc/"stat")
    cpus=cpu_stat(source)
    memory=ints(read(proc/"meminfo"),":")
    vm=ints(read(proc/"vmstat"))
    net=paired(read(proc/"net/snmp"))
    net.update(paired(read(proc/"net/netstat")))
    active=("Tcp:RetransSegs","Tcp:InSegs","Tcp:OutSegs","TcpExt:ListenOverflows",
            "TcpExt:ListenDrops","Ip:InDiscards","Ip:OutDiscards")
    counts={k:net[k] for k in active if k in net}
    counts.update({k:vm[k] for k in ("pgfault","pgmajfault","pswpin","pswpout","oom_kill") if k in vm})
    summary=ints(source)
    counts.update({k:summary[k] for k in ("ctxt","processes","intr") if k in summary})
    gauges={k:summary[k] for k in ("procs_running","procs_blocked") if k in summary}
    gauges.update({k:memory[k] for k in ("MemTotal","MemAvailable","SwapTotal","SwapFree") if k in memory})
    for resource in ("cpu","memory","io"):
        for line in read(proc/"pressure"/resource).splitlines():
            parts=line.split()
            if parts and parts[0] in ("some","full"):
                for item in parts[1:]:
                    if item.startswith("avg10="):
                        gauges["psi_"+resource+"_"+parts[0]]=float(item[6:])
    sched_delay=read(proc/"sys/kernel/sched_schedstats").strip()=="1"
    processes={}
    # Sample declared roots first so a busy service's children cannot hide another service.
    pending=deque((service,pid,True) for service,pid in list(targets.items())[:LIMIT_PROCESSES])
    while pending and len(processes)<LIMIT_PROCESSES:
        service,ident,root=pending.popleft()
        if ident in processes:
            continue
        path=proc/str(ident)
        try:
            p=inspect_process(path,ident,service,details,sched_delay,memory.get("MemTotal"))
        except (ValueError,IndexError):
            continue
        if p is None:
            continue
        processes[ident]=p
        new=[int(n) for n in read(path/"task"/str(ident)/"children").split() if n.isdecimal()]
        pending.extend((service,child,False) for child in new[:LIMIT_PROCESSES])
        if root and details:
            try:
                tids=sorted(int(t.name) for t in (path/"task").iterdir() if t.name.isdecimal())
            except OSError:
                tids=[]
            p["threadStats"]=[]
            for tid in tids[:LIMIT_THREADS]:
                try:
                    entry=inspect_process(path/"task"/str(tid),tid,sched_delay=sched_delay)
                    if entry:
                        p["threadStats"].append(entry)
                except (ValueError,IndexError):
                    continue
            p["threadTruncated"]=len(tids)>LIMIT_THREADS
    return {"at":time.time(),"clock":time.monotonic(),"host":socket.gethostname(),"kernel":os.uname().release if hasattr(os,"uname") else "",
            "cpus":cpus,"counters":counts,"gauges":gauges,"processes":list(processes.values()),"schedstats":sched_delay}


def interval(now,old,detail=True):
    elapsed=now["clock"]-old["clock"] if old else 0
    cpus={}
    total=busy=0
    for key,value in now["cpus"].items():
        prior=old["cpus"].get(key,{}) if old else {}
        readings,ticks=proportions(value,prior)
        cpus[key]=readings
        if key=="cpu":
            total=ticks
            busy=total-(delta(value,prior,"idle") or 0)-(delta(value,prior,"iowait") or 0)-(delta(value,prior,"steal") or 0)
    samples={}
    if old and elapsed>0:
        for key in now["counters"]:
            value=delta(now["counters"],old["counters"],key)
            samples[key]=quantize(value/elapsed) if value is not None else None
    prior={p["key"]:p for p in old["processes"]} if old else {}
    processes=[]
    for p in now["processes"]:
        q=prior.get(p["key"])
        current={k:p.get(k) for k in ("key","pid","name","state","service","threads","affinity","lastCpu","rssKiB","pssKiB","swapKiB","rssPct","fds")}
        current.update(process_rates(p,q,elapsed,total,busy))
        if detail and "threadStats" in p:
            earlier={t["key"]:t for t in q.get("threadStats",[])} if q else {}
            current["threadStats"]=[{**{k:t[k] for k in ("pid","name","state","lastCpu","affinity")},
                                     **process_rates(t,earlier.get(t["key"]),elapsed)}
                                    for t in p["threadStats"]]
            current["threadTruncated"]=p["threadTruncated"]
        processes.append(current)
    return {"at":now["at"],"interval":round(max(0,elapsed),3),"host":now["host"],"kernel":now["kernel"],
            "logicalCpuCount":sum(key!="cpu" for key in cpus) or None,
            "cpu":cpus,"gauges":now["gauges"],"rates":samples,"processes":processes,"schedstats":now["schedstats"]}
