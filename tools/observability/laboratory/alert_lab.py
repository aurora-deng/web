"""Reproduce real 5xx and a slow PostgreSQL operation in a disposable Linux lab."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import http.client
import json
from pathlib import Path
import secrets
import subprocess
import time

from tools.observability.laboratory.lab import ROOT, prepare_credentials
from tools.observability.observer.metrics.local_metrics import LocalMetrics
from tools.observability.observer.storage.store import Store
from tools.observability.laboratory.probe import process_threads
from repro_runtime import browser_fixture, clean_environment, disposable_database, owned_server_process, require


def read_conversations(port,cookie):
    conn=http.client.HTTPConnection("127.0.0.1",port,timeout=15)
    try:
        conn.request("GET","/api/conversations",headers={"Cookie":cookie})
        response=conn.getresponse()
        response.read(65536)
        return response.status
    finally:
        conn.close()


def sql(prefix,connection,statement):
    return subprocess.check_output([str(prefix/"bin/psql"),"-XqAt","-w","-d",connection,"-c",statement],
                                   env=clean_environment(),text=True,timeout=5).strip()


def wait_for(action,seconds=10):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        if value:=action():
            return value
        time.sleep(.15)
    raise RuntimeError("Condition did not become available within the limit")


@contextmanager
def hold_conversations(prefix,connection):
    # The DSN is supplied by disposable_database; all locks belong to this fixture.
    a=[str(prefix/"bin/psql"),"-XqAt","-w","-d",connection+" application_name=observer_alert_lock"]
    with subprocess.Popen(a,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                          text=True,env=clean_environment()) as holder:
        try:
            holder.stdin.write("BEGIN; LOCK TABLE conversations IN ACCESS EXCLUSIVE MODE;\n")
            holder.stdin.flush()
            wait_for(lambda:sql(prefix,connection,
                "SELECT 1 FROM pg_locks JOIN pg_stat_activity USING (pid) WHERE granted "
                "AND relation='conversations'::regclass AND mode='AccessExclusiveLock' "
                "AND application_name='observer_alert_lock' LIMIT 1"))
            yield
        finally:
            if holder.poll() is None:
                holder.stdin.write("ROLLBACK;\n")
                holder.stdin.close()
                try:
                    holder.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    holder.terminate()
                    holder.wait(timeout=6)


def exercise(prefix,connection,port,cookie,sampler):
    statements=(" WHERE datname=current_database() AND wait_event_type='Lock' "
                "AND query ILIKE '%conversations%' AND pid<>pg_backend_pid()")
    def waiter():
        return sql(prefix,connection,"SELECT pid FROM pg_stat_activity"+statements+" LIMIT 1")
    readings=[]
    raw=0
    with ThreadPoolExecutor(max_workers=1) as workers:
        with hold_conversations(prefix,connection):
            pending=workers.submit(read_conversations,port,cookie)
            wait_for(waiter)
            time.sleep(2.25)
        require(pending.result(timeout=12)==200,"Slow request did not recover")
        def inject():
            with hold_conversations(prefix,connection):
                # Each cancelled request has a live PostgreSQL waiter and produces a 5xx.
                for _ in range(4):
                    pending=workers.submit(read_conversations,port,cookie)
                    ident=int(wait_for(waiter))
                    require(sql(prefix,connection,"SELECT pg_cancel_backend("+str(ident)+")")=="t","DB cancel failed")
                    require(500<=pending.result(timeout=12)<600,"Expected a real HTTP server error")

        inject()
        raw+=4
        launched=time.monotonic()
        extra=False
        previous={}
        deadline=launched+340
        while time.monotonic()<deadline:
            if not extra and time.monotonic()-launched>=45:
                inject()
                raw+=4
                extra=True
            current={}
            for row in sampler.prom("alerts")["alerts"]:
                name=row["labels"]["alertname"]
                if not name.startswith(("NebulaHttpHighErrors","NebulaDatabaseMaxSlow")):
                    continue
                current[name]=row["state"]
                if previous.get(name)!=row["state"]:
                    readings.append({"rule":name,"state":row["state"],"value":row.get("value"),
                                     "elapsedSeconds":round(time.monotonic()-launched,2)})
            previous=current
            if all(current.get(rule)=="firing" for rule in ("NebulaHttpHighErrors","NebulaDatabaseMaxSlow")):
                return {"real5xx":raw,"slowSucceeded":True,"transitions":readings}
            if read_conversations(port,cookie)!=200:
                raise RuntimeError("A regular observation request did not recover")
            time.sleep(8)
        raise RuntimeError("Expected alerts did not both fire: "+json.dumps(readings,ensure_ascii=False))


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    for name in ("binary","postgres-prefix","output"):
        cli.add_argument("--"+name,type=Path,required=True)
    args=cli.parse_args()
    path=args.output.resolve()
    path.mkdir(mode=0o700,parents=True,exist_ok=False)
    prefix=args.postgres_prefix.resolve()
    credential=prepare_credentials(path)
    with disposable_database(prefix,ROOT,path) as (connection,_):
        env={**clean_environment(),"WEB_PHASE11_REQUIRED":"1","WEB_FUNCTION3_ENABLE":"1",
             "WEB_PHASE11_DATABASE":connection,"WEB_PHASE11_TOKEN_KEY_FILE":str(path/"token.key"),
             "WEB_PHASE11_FILE_ROOT":str(path/"files"),"WEB_FUNCTION3_MEDIA_ROOT":str(path/"media"),
             "WEB_OBSERVABILITY":"1","WEB_AUTH_SECRET":credential}
        with owned_server_process(args.binary.resolve(),ROOT,env,path/"server.log") as (port,process):
            fixture=browser_fixture(ROOT).BrowserClient("127.0.0.1",port)
            fixture.register_and_login("obs_alert_"+secrets.token_hex(6),secrets.token_urlsafe(20))
            sampler=LocalMetrics(port,(path/"scrape-token").read_text(),process.pid,process_threads,Store(path/"alerts.sqlite"))
            with sampler:
                # Establish a counter baseline before the first error.
                wait_for(lambda:sampler.prom("query",{"query":'up{job="nebula"}'})["result"],20)
                evidence=exercise(prefix,connection,port,fixture.cookie,sampler)
    (path/"report.json").write_text(json.dumps(evidence,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(evidence,ensure_ascii=False),flush=True)


if __name__=="__main__":
    main()
