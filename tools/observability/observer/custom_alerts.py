"""Evaluate user-defined threshold rules from the same series used by dashboards."""
import json
import operator
import sqlite3
import threading
import time

from .events import Invalid
from .sources import BackendError
from .store import encode


RULE_LIMIT = 12
INTERVAL = 10
HISTORY_SECONDS = 7 * 86400
OPS = {"gt":operator.gt, "gte":operator.ge, "lt":operator.lt, "lte":operator.le}
REDUCERS = {"last":lambda values:values[-1], "avg":lambda values:sum(values)/len(values),
            "max":max, "min":min}


def inspect_series(rule, reply, now):
    """Return the value and sample count per fresh series in the selected window."""
    latest = now-max(15,2*reply.get("step",5))
    results=[]
    for series in reply["series"]:
        points=[(stamp,value) for stamp,value in series["points"] if value is not None and abs(value)<=1e20]
        if not points or points[-1][0]<latest:
            continue
        sample=points[-1][0]
        value=REDUCERS[rule["reducer"]]([reading for _,reading in points])
        results.append({"labels":series["labels"],"value":value,"sampleAt":sample,"samples":len(points),
                        "breached":OPS[rule["op"]](value,rule["threshold"])})
    return results


def advance(rule, readings, previous, now, stale_after):
    current={}
    rows=[]
    for reading in readings:
        seen, state = reading["sampleAt"], "normal"
        since=None
        if reading["breached"]:
            key=encode(reading["labels"])
            first, last = previous.get(key,(seen,seen))
            since=seen if seen<last or seen-last>stale_after else first
            current[key]=since,seen
            state="firing" if seen-since>=rule["pendingSeconds"] else "pending"
        rows.append({**reading,"state":state,"activeSince":since})
    for state in ("firing","pending","normal"):
        choices=[row for row in rows if row["state"]==state]
        if choices:
            best=(max if rule["op"].startswith("g") else min)(choices,key=lambda row:row["value"])
            return {**best,"checkedAt":now,"series":rows},current
    return {"state":"no_data","checkedAt":now,"series":[]},{}


class CustomAlerts:
    def __init__(self, workspace, monitor):
        self.workspace,self.monitor=workspace,monitor
        self.store=workspace.store
        self.current={}
        self.lock=threading.Lock()
        self.done=threading.Event()
        self.worker=threading.Thread(target=self.run,name="observer-alerts")
        with self.store.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS alert_transitions(
                  id INTEGER PRIMARY KEY, stamp INTEGER NOT NULL, body TEXT NOT NULL);
            """)

    def start(self):
        self.worker.start()

    def close(self):
        self.done.set()
        if self.worker.ident is not None:
            self.worker.join()

    def read(self, rule, now):
        reply=self.monitor.query(rule,now-rule["windowSeconds"],now)
        return inspect_series(rule,reply,now),max(2*INTERVAL,reply.get("step",5)*2+5),reply.get("truncated",False)

    def preview(self, rule):
        now=int(time.time())
        readings,gap,partial=self.read(rule,now)
        result,_=advance(rule,readings,{},now,gap)
        return {**result,"partial":partial}

    def view(self):
        rules=self.workspace.list("alert")
        with self.lock:
            current=dict(self.current)
        result=[]
        for rule in rules:
            old=current.get(rule["id"])
            state={"state":"paused" if not rule["enabled"] else "waiting"}
            if old and old[0]==rule["revision"] and rule["enabled"]:
                state=old[1]
            result.append({**rule,"result":state})
        return result

    def history(self):
        with self.store.read() as db:
            return [json.loads(row["body"]) for row in db.execute(
                "SELECT body FROM alert_transitions WHERE stamp>=? ORDER BY id DESC LIMIT 60",
                (int(time.time())-HISTORY_SECONDS,))]

    def evaluate(self):
        rows, transitions = {}, []
        for rule in self.workspace.list("alert"):
            if self.done.is_set():
                break
            ident, revision, now = rule["id"],rule["revision"],int(time.time())
            old = self.current.get(ident)
            continuous=old and old[0]==revision and now-old[1]["checkedAt"]<=2*INTERVAL
            timers=old[2] if continuous else {}
            if not rule["enabled"]:
                result, timers = {"state":"paused","checkedAt":now},{}
            else:
                try:
                    readings,gap,partial=self.read(rule,now)
                    result,timers=advance(rule,readings,timers,now,gap)
                    result["partial"]=partial
                except (BackendError,Invalid,ValueError,OSError) as error:
                    result,timers={"state":"error","checkedAt":now,"error":str(error)[:180]},{}
            rows[ident]=(revision,result,timers)
            prior=old[1]["state"] if old else None
            if prior!=result["state"] and (prior is not None or result["state"] in ("pending","firing","error")):
                transitions.append((now,encode({"ruleId":ident,"name":rule["name"],"at":now,
                    "state":result["state"],"value":result.get("value"),"labels":result.get("labels"),"unit":rule["unit"]})))
        with self.lock:
            self.current=rows
        if transitions:
            with self.store.connect() as db:
                db.executemany("INSERT INTO alert_transitions(stamp,body) VALUES (?,?)",transitions)
                db.execute("DELETE FROM alert_transitions WHERE stamp<? OR id IN "
                           "(SELECT id FROM alert_transitions ORDER BY id DESC LIMIT -1 OFFSET 256)",
                           (int(time.time())-HISTORY_SECONDS,))

    def run(self):
        while not self.done.is_set():
            start=time.monotonic()
            try:
                self.evaluate()
            except sqlite3.OperationalError:
                # Keep serving the workspace while SQLite is locked or temporarily unavailable.
                pass
            self.done.wait(max(0.5,INTERVAL-(time.monotonic()-start)))
