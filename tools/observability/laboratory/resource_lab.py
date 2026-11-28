"""Isolated CPU-affinity, syscall and heap/function experiment, no kernel changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time
from types import SimpleNamespace

from tools.observability.observer.resources.linux_resources import interval, snapshot
from tools.observability.diagnostics.profile_formats import callgrind, dhat, syscalls

ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT/"scripts/project"),str(ROOT/"benchmarks")]
from repro_runtime import clean_environment, disposable_database, owned_server_process, require
from phase15_business_benchmark import BusinessBenchmark, WebSocketClient


def parse_core_set(text):
    cpus=set()
    for option in text.split(","):
        unit=option.split("-",1)
        if not all(part.isdecimal() for part in unit):
            raise ValueError("CPU affinity must contain numeric ranges")
        a,b=int(unit[0]),int(unit[-1])
        if not 0<=a<=b<4096:
            raise ValueError("CPU range out of bounds")
        cpus.update(range(a,b+1))
    return cpus


def observations(port, count, tag, sample):
    bench=BusinessBenchmark(SimpleNamespace(host="127.0.0.1",port=port,timeout=20))
    pair=bench.create_pair("resource")
    client=WebSocketClient(pair.sender.api)
    client.connect()
    results=[]
    pending=[]
    sample_at=time.monotonic()
    try:
        for i in range(count):
            ident=f"{tag}-{i}-{os.getpid()}"
            start=time.perf_counter_ns()
            client.send_json({"v":1,"type":"chat.send","id":ident,
                              "conversationId":str(pair.conversation),"content":"Resource lab fixture"})
            accepted=client.wait("chat.accepted",ident)
            require(accepted["status"]=="inserted","Chat write was not confirmed")
            reading=(time.perf_counter_ns()-start)/1e6
            results.append(reading)
            pending.append(reading)
            if i%12==0:
                status,_=pair.receiver.api.request("GET",f"/api/conversations/{pair.conversation}/messages?after=0&limit=20")
                require(status==200,"History request failed")
            if i%5==0:
                status,_=pair.sender.api.request("GET","/api/conversations")
                require(status==200,"Read failed")
            if time.monotonic()-sample_at>=1 or i==count-1:
                sample(sorted(pending))
                pending.clear()
                sample_at=time.monotonic()
    finally:
        client.close()
        pair.sender.api.close()
        pair.receiver.api.close()
    return sorted(results)


def summary(timings):
    def q(p):
        return round(timings[max(0,min(len(timings)-1,int(len(timings)*p+.9999)-1))],3)
    return {"samples":len(timings),"p50Ms":q(.5),"p95Ms":q(.95),"p99Ms":q(.99),"maxMs":round(timings[-1],3)}


def child_target(pid):
    path=Path(f"/proc/{pid}/task/{pid}/children")
    nodes=path.read_text().split()
    return int(nodes[0]) if nodes else pid


def leg(args,path,connection,mode,round_no):
    path.mkdir()
    key=path/"token.key"
    key.touch(mode=0o600,exist_ok=False)
    key.write_text(os.urandom(32).hex(),encoding="ascii")
    env={**clean_environment(),"WEB_PHASE11_REQUIRED":"1","WEB_FUNCTION3_ENABLE":"1","WEB_PHASE11_DATABASE":connection,
         "WEB_PHASE11_TOKEN_KEY_FILE":str(key),"WEB_PHASE11_FILE_ROOT":str(path/"files"),
         "WEB_FUNCTION3_MEDIA_ROOT":str(path/"media"),"WEB_OBSERVABILITY":"1"}
    prefix=[]
    check_readiness=20
    if mode=="pinned":
        prefix=["taskset","-c",args.cpus]
    if mode=="syscalls":
        prefix=["strace","-f","-c","-w","-o",str(path/"syscalls.txt")]
    if mode in ("callgrind","dhat"):
        flags=["--collect-systime=nsec","--callgrind-out-file="+str(path/"functions.out")] if mode=="callgrind" else ["--dhat-out-file="+str(path/"heap.json")]
        prefix=["valgrind","-q","--tool="+mode,*flags]
        check_readiness=55
    started=int(time.time())
    with owned_server_process(args.binary,ROOT,env,path/"server.log",prefix=prefix,startup_seconds=check_readiness) as (port,process):
        pid=child_target(process.pid) if mode=="syscalls" else process.pid
        socket_dir=Path(connection.split()[0].removeprefix("host="))
        pg_pid=int((socket_dir/"data/postmaster.pid").read_text().splitlines()[0])
        targets={"NEBULA":pid,"PostgreSQL":pg_pid}
        previous=snapshot(targets,details=False)
        last=previous
        timeline=[]
        def sample(timings):
            nonlocal last
            current=snapshot(targets,details=False)
            row=interval(current,last,detail=False)
            service=next((p for p in row["processes"] if p["service"]=="NEBULA"),{})
            timeline.append({"at":row["at"],"seconds":row["interval"],"ack":summary(timings),
                             "cpuPct":service.get("cpuPct"),"systemPct":service.get("systemPct"),
                             "rssKiB":service.get("rssKiB"),"contextSwitches":row["rates"].get("ctxt"),
                             "retransmits":row["rates"].get("Tcp:RetransSegs"),
                             "majorFaults":row["rates"].get("pgmajfault")})
            last=current
        count=args.requests if mode in ("plain","pinned") else min(35,args.requests)
        wall=time.monotonic()
        results=observations(port,count,mode,sample)
        duration=time.monotonic()-wall
        current=snapshot({"NEBULA":pid,"PostgreSQL":pg_pid},details=True)
        proc_snapshot=interval(current,previous,detail=False)
        proc_snapshot["cpu"]={"cpu":proc_snapshot["cpu"].get("cpu",{})}
        for resource in proc_snapshot["processes"]:
            resource.pop("threadStats",None)
    run={"mode":mode,"round":round_no,"started":started,"finished":int(time.time()),"wallSeconds":round(duration,3),
         "ack":summary(results),"resource":proc_snapshot,
         "timeline":timeline,"affinity":args.cpus if mode=="pinned" else "inherited","instrumented":mode not in ("plain","pinned")}
    if mode=="syscalls":
        run["syscalls"]=syscalls(path/"syscalls.txt")
    elif mode=="callgrind":
        run["functions"]=callgrind(path/"functions.out")
    elif mode=="dhat":
        run["heap"]=dhat(path/"heap.json")
    print(json.dumps({"mode":mode,"round":round_no,**run["ack"]}),flush=True)
    return run


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary",required=True,type=Path)
    p.add_argument("--postgres-prefix",required=True,type=Path)
    p.add_argument("--output",required=True,type=Path)
    p.add_argument("--cpus",required=True,help="Server CPUs for the pinned leg only, e.g. 2-3")
    p.add_argument("--rounds",type=int,default=3)
    p.add_argument("--requests",type=int,default=250)
    p.add_argument("--skip-instrumented",action="store_true")
    args=p.parse_args()
    require(sys.platform.startswith("linux") and os.geteuid()>0,"Use an unprivileged Linux account")
    require(1<=args.rounds<=5 and 30<=args.requests<=800,"Invalid experiment size")
    require(bool(parse_core_set(args.cpus)) and parse_core_set(args.cpus)<=os.sched_getaffinity(0),"Requested CPUs are not available")
    args.binary=args.binary.resolve()
    output=args.output.resolve()
    output.mkdir(mode=0o700,parents=True,exist_ok=False)
    tools={key:shutil.which(key) for key in ("taskset","strace","valgrind","perf")}
    require(tools["taskset"],"taskset must already be installed")
    started=int(time.time())
    runs=[]
    skipped={}
    plan=[(mode,number) for number in range(1,args.rounds+1)
          for mode in (("plain","pinned") if number%2 else ("pinned","plain"))]
    if not args.skip_instrumented:
        plan.extend((mode,1) for mode in ("syscalls","callgrind","dhat"))
    for mode,number in plan:
        tool="strace" if mode=="syscalls" else "valgrind" if mode in ("callgrind","dhat") else None
        if tool and not tools[tool]:
            skipped[mode]=tool+" unavailable"
            continue
        path=output/f"{mode}-{number}"
        db_dir=output/f"db-{mode}-{number}"
        db_dir.mkdir()
        try:
            with disposable_database(args.postgres_prefix.resolve(),ROOT,db_dir) as (connection,_):
                runs.append(leg(args,path,connection,mode,number))
        except (RuntimeError,OSError,subprocess.SubprocessError) as error:
            skipped[mode+"-"+str(number)]=str(error).splitlines()[0][:240]
            print(json.dumps({"mode":mode,"error":skipped[mode+"-"+str(number)]}),flush=True)
    baseline={}
    for mode in ("plain","pinned"):
        values=[run["ack"]["p99Ms"] for run in runs if run["mode"]==mode]
        baseline[mode]={"p99Ms":statistics.median(values),"rounds":len(values)} if values else None
    require(bool(runs),"All resource legs failed")
    data={"schema":"nebula-resource-lab/1","id":output.name,"label":"NEBULA CPU 亲和性、内核与函数诊断",
          "started":started,"finished":int(time.time()),"kernel":os.uname().release,
          "binarySha256":hashlib.sha256(args.binary.read_bytes()).hexdigest(),"tools":tools,
          "baseline":baseline,"runs":runs,"skipped":skipped}
    (output/"report.json").write_text(json.dumps(data,ensure_ascii=False,allow_nan=False,separators=(",",":")),encoding="utf-8")
    if skipped:
        print(json.dumps({"incomplete":skipped}),flush=True)


if __name__=="__main__":
    main()
