"""Bounded machine and process history plus normalized profiling reports."""
import json
import math
import re
import time

from .events import Invalid
from .commit_report import normalize_commit
from .linux_resources import interval, snapshot
from .store import encode

DETAIL_SAMPLES=12
PROFILE_LIMIT=24
PROFILE_BYTES=2*1024*1024
PROFILE_ID=re.compile(r"[a-zA-Z0-9_.-]{1,80}")


def ack_breakdown(path,binary_sha):
    if path.stat().st_size>PROFILE_BYTES:
        raise Invalid("ACK 分段报告超过 2 MiB")
    report=json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(report,dict) or report.get("mode")!="profile" or report.get("passed") is not True
            or not isinstance(binary_sha,str) or not re.fullmatch("[0-9a-f]{64}",binary_sha)
            or report.get("binarySha256")!=binary_sha):
        raise Invalid("ACK 分段报告必须为同一二进制的成功 profile 实验")
    fields=("queueUs","poolAcquireUs","commitStartUs","commitUs","ackReadyUs")
    def value(source,key):
        sample=source[key]
        if type(sample) not in (int,float) or not math.isfinite(sample):
            raise ValueError(key)
        return sample
    try:
        run=report["instrumented"]
        trace=run["trace"]
        if trace["serverUnits"]!="microseconds":
            raise ValueError("serverUnits")
        return {"workers":value(run["configuration"],"workers"),"paired":value(trace,"paired"),
                "rttP99Ms":value(run["durableChatAckMs"],"p99"),
                "stageP99Us":{field:value(trace["serverQuantiles"][field],"p99") for field in fields},
                "slowest":[{key:value(row,key) for key in ("rttMs",*fields)}
                           for row in trace["slowestOnePercent"][:40]]}
    except (KeyError,TypeError,ValueError,OverflowError):
        raise Invalid("ACK 分段字段无效") from None


class ResourceHistory:
    def __init__(self,store,targets):
        self.store,self.targets=store,dict(targets)
        self.started={}
        self.previous=None
        with store.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS resource_snapshots(
                    stamp INTEGER PRIMARY KEY, body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS resource_profiles(
                    id TEXT PRIMARY KEY, started INTEGER NOT NULL, body TEXT NOT NULL);
            """)

    def capture(self):
        raw=snapshot(self.targets)
        # Bind configured service identity to its start time; never follow a reused PID.
        for process in raw["processes"]:
            if self.targets.get(process["service"])==process["pid"]:
                self.started.setdefault(process["service"],process["start"])
                if self.started[process["service"]]!=process["start"]:
                    self.targets.pop(process["service"])
        raw["processes"]=[p for p in raw["processes"] if p["service"] in self.targets]
        sample=interval(raw,self.previous)
        self.previous=raw
        stamp=int(sample["at"])
        recent=encode(sample)
        # Retain detail on recent samples so drill-down survives a refresh or a pinned window.
        with self.store.connect() as db:
            prior=db.execute("SELECT stamp,body FROM resource_snapshots ORDER BY stamp DESC LIMIT 1 OFFSET ?",
                             (DETAIL_SAMPLES-1,)).fetchone()
            if prior:
                old=json.loads(prior["body"])
                for process in old["processes"]:
                    process.pop("threadStats",None)
                    process.pop("threadTruncated",None)
                old["cpu"]={"cpu":old["cpu"].get("cpu",{})}
                db.execute("UPDATE resource_snapshots SET body=? WHERE stamp=?",(encode(old),prior["stamp"]))
            db.execute("INSERT OR REPLACE INTO resource_snapshots VALUES (?,?)",(stamp,recent))

    def history(self,start,end):
        start=max(start,int(time.time())-self.store.retention_seconds)
        span=max(1,end-start)
        step=max(1,(span+179)//180)
        with self.store.connect() as db:
            rows=db.execute("SELECT body FROM resource_snapshots WHERE stamp IN ("
                            "SELECT max(stamp) FROM resource_snapshots WHERE stamp BETWEEN ? AND ? "
                            "GROUP BY (stamp-?)/?) ORDER BY stamp",(start,end,start,step)).fetchall()
            last=db.execute("SELECT body FROM resource_snapshots WHERE stamp BETWEEN ? AND ? ORDER BY stamp DESC LIMIT 1",
                            (start,end)).fetchone()
        result=[]
        for row in rows:
            item=json.loads(row["body"])
            item["cpu"]={"cpu":item["cpu"].get("cpu",{})}
            for process in item["processes"]:
                process.pop("threadStats",None)
            result.append(item)
        latest=json.loads(last["body"]) if last else None
        return {"status":"ok" if result else "empty","latest":latest,"series":result}

    def import_file(self,path,ack_profile=None):
        if path.stat().st_size>PROFILE_BYTES:
            raise Invalid("分析报告超过 2 MiB")
        report=json.loads(path.read_text(encoding="utf-8"))
        ident=report.get("id") if isinstance(report,dict) else None
        if (not isinstance(ident,str) or not PROFILE_ID.fullmatch(ident)
                or report.get("schema")!="nebula-resource-lab/1"
                or not isinstance(report.get("label"),str) or not 1<=len(report["label"])<=120
                or type(report.get("started")) is not int or type(report.get("finished")) is not int
                or not int(time.time())-self.store.retention_seconds <= report["started"] <= report["finished"] <= int(time.time())+60
                or not isinstance(report.get("runs"),list) or not 1<=len(report["runs"])<=24):
            raise Invalid("资源报告格式无效")
        if ack_profile is not None:
            report["ackBreakdown"]=ack_breakdown(ack_profile,report.get("binarySha256"))
        report.pop("kind",None)
        return self.save_report(report)

    def import_commit(self,path,ident,label):
        label=label.strip()
        if not PROFILE_ID.fullmatch(ident) or not 1<=len(label)<=120:
            raise Invalid("诊断报告 ID 或名称无效")
        with path.open("rb") as source:
            payload=source.read(PROFILE_BYTES+1)
        if len(payload)>PROFILE_BYTES:
            raise Invalid("提交诊断报告超过 2 MiB")
        try:
            report=json.loads(payload.decode("utf-8"))
        except (ValueError,RecursionError):
            raise Invalid("提交诊断报告需使用 UTF-8 JSON") from None
        now=int(time.time())
        return self.save_report({"kind":"commit","id":ident,"label":label,"started":now,"finished":now,
                                 **normalize_commit(report)})

    def save_report(self,report):
        ident=report["id"]
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            previous=db.execute("SELECT body FROM resource_profiles WHERE id=?",(ident,)).fetchone()
            if previous and json.loads(previous["body"]).get("kind")!=report.get("kind"):
                raise Invalid("此 ID 已归档为其他类型的报告")
            db.execute("INSERT OR REPLACE INTO resource_profiles VALUES (?,?,?)",(ident,report["started"],encode(report)))
            db.execute("DELETE FROM resource_profiles WHERE id IN (SELECT id FROM resource_profiles ORDER BY started DESC,id LIMIT -1 OFFSET ?)",
                       (PROFILE_LIMIT,))
        return ident

    def reports(self,ident=None):
        floor=int(time.time())-self.store.retention_seconds
        with self.store.connect() as db:
            if ident:
                if not PROFILE_ID.fullmatch(ident):
                    raise Invalid("报告 ID 无效")
                row=db.execute("SELECT body FROM resource_profiles WHERE id=? AND started>=?",(ident,floor)).fetchone()
                return json.loads(row["body"]) if row else None
            return [{k:value[k] for k in ("id","started","finished","label","kind") if k in value}
                    for row in db.execute("SELECT body FROM resource_profiles WHERE started>=? ORDER BY started DESC,id",(floor,))
                    if (value:=json.loads(row["body"]))]
