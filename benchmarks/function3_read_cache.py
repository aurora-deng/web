"""Bounded read-only cache comparison on phase11; controls only service.py's owned instance.

Uses existing demo credentials without logging them. Never creates databases,
users or business content, and never stops/reconfigures the shared Redis service.
"""
import concurrent.futures
import http.client
import json
from pathlib import Path
import statistics
import subprocess
import time
source=Path(__file__).resolve().parents[1]
controller=source/'scripts/function3/runtime/service.py'
out=Path('/home/pikachu/phase11-data/function3/validation/phase35')
pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
dsn='host=/home/pikachu/phase11-data/postgresql/run port=5432 user=pikachu dbname=phase11'
password=Path('/home/pikachu/phase11-data/secrets/phase11-demo.password').read_text().strip()
def control(*args):subprocess.run(['python3',str(controller),*args],stdout=subprocess.DEVNULL,check=True)
def commits():return int(subprocess.check_output([pg,dsn,'-w','-X','-Atc',"SELECT xact_commit FROM pg_stat_database WHERE datname=current_database()"],text=True).strip())
def login():
    c=http.client.HTTPConnection('127.0.0.1',18090,timeout=10)
    c.request('POST','/api/auth/login',json.dumps({'username':'nebula_owner','password':password}),{'Content-Type':'application/json','Origin':'http://127.0.0.1:18090'})
    r=c.getresponse();r.read();assert r.status==200;cookie=r.getheader('Set-Cookie').split(';')[0];c.close();return cookie
def run(cookie,seconds=8):
    c=http.client.HTTPConnection('127.0.0.1',18090,timeout=5);times=[];errors=0
    deadline=time.perf_counter()+seconds
    while time.perf_counter()<deadline:
        start=time.perf_counter();c.request('GET','/api/tree/categories',headers={'Cookie':cookie});r=c.getresponse();r.read();errors+=r.status!=200;times.append((time.perf_counter()-start)*1000)
    c.close();return times,errors
def round_value(value):return round(value,3)
report={'scope':'GET /api/tree/categories only, 4 workers x 8 seconds per round; same binary; AI/TTS disabled equally during measurement; AB/BA/AB order','rounds':[]}
try:
    for cycle in range(3):
        for enabled in ([False,True] if cycle%2==0 else [True,False]):
            control('stop');control('start','--no-ai','--no-speech',*(() if enabled else ('--no-redis',)))
            cookie=login()
            c=http.client.HTTPConnection('127.0.0.1',18090,timeout=5)
            for _ in range(16):c.request('GET','/api/tree/categories',headers={'Cookie':cookie});r=c.getresponse();r.read();assert r.status==200
            c.close();time.sleep(1.1);before=commits();begin=time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(4) as pool:results=list(pool.map(lambda _:run(cookie),range(4)))
            elapsed=time.perf_counter()-begin;times=sorted(t for ts,_ in results for t in ts);time.sleep(1.1);after=commits()
            row={'round':cycle+1,'redis':enabled,'requests':len(times),'errors':sum(e for _,e in results),'seconds':round_value(elapsed),'qps':round_value(len(times)/elapsed),'p50Ms':round_value(statistics.median(times)),'p95Ms':round_value(times[int(len(times)*.95)-1]),'databaseCommitDelta':after-before,'databaseCommitsPerRequest':round_value((after-before)/len(times))}
            report['rounds'].append(row)
            (out/'read-cache-benchmark.json').write_text(json.dumps(report,indent=2)+'\n')
    control('stop');control('start','--no-ai','--no-speech','--redis-port','1');cookie=login();times,errors=run(cookie,2)
    report['unavailableEndpoint']={'requests':len(times),'errors':errors,'p95Ms':round_value(sorted(times)[int(len(times)*.95)-1]),'sharedRedisStopped':False}
finally:
    control('stop');control('start')
    (out/'read-cache-benchmark.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
