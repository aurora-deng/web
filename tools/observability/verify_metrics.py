"""Verify approved real metrics backends and an owned lab's failure isolation."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from .observer.access_preflight import metrics_access


def require(value,message):
    if not value:raise RuntimeError(message)


def request(base,path,params=None):
    url=base+path+('?' + urllib.parse.urlencode(params) if params else '')
    try:
        with urllib.request.urlopen(url,timeout=5) as reply:return reply.status,json.load(reply)
    except urllib.error.HTTPError as error:return error.code,json.load(error)


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--run-dir',type=Path,required=True);cli.add_argument('--output',type=Path,required=True)
    cli.add_argument('--exercise-outage',action='store_true')
    args=cli.parse_args();run=args.run_dir.resolve();session=json.loads((run/'session.json').read_text())
    require(sys.platform.startswith('linux') and session['profile']=='metrics','Use an owned Linux metrics lab')
    owner=int((run/'lab.pid').read_text());command=Path(f'/proc/{owner}/cmdline').read_bytes().split(b'\0')
    require(b'tools.observability.lab' in command and str(run).encode() in command,'Lab ownership mismatch')
    children=Path(f'/proc/{owner}/task/{owner}/children').read_text().split()
    processes={Path(f'/proc/{pid}/exe').resolve().name:int(pid) for pid in children}
    require({'prometheus','node_exporter','webserver'}<=processes.keys(),'Expected real processes missing')
    base='http://127.0.0.1:'+str(session['ports']['observer'])
    prom='http://127.0.0.1:'+str(session['ports']['prometheus'])
    access=metrics_access(session['nebulaPort'],(run/'scrape-token').read_text().strip(),(run/'member-token').read_text().strip())
    expression='histogram_quantile(0.99,sum by(le)(rate(nebula_function_seconds_bucket{function="ChatService::sendMessage"}[5m])))'
    deadline=time.monotonic()+40
    while True:
        code,targets=request(prom,'/api/v1/targets')
        code,quantile=request(prom,'/api/v1/query',{'query':expression})
        values=[float(row['value'][1]) for row in quantile.get('data',{}).get('result',[])]
        if values and all(math.isfinite(value) and value>0 for value in values):break
        require(time.monotonic()<deadline,'No real function histogram samples; exercise chat then wait for scrapes')
        time.sleep(.25)
    targets=targets['data']['activeTargets']
    require({'nebula','node','probe','observer','prometheus'} <= {row['labels']['job'] for row in targets},'Scrape jobs missing')
    require(all(row['health']=='up' for row in targets),'A real scrape target is down')
    nebula=next(row['labels'] for row in targets if row['labels']['job']=='nebula')
    require(nebula['instance']=='127.0.0.1:'+str(session['nebulaPort']) and {'bc','app'}<=nebula.keys(),'Discovery labels or target are stale')
    now=int(time.time());code,instrument=request(base,'/api/v1/instrumentation',{'from':now-900,'to':now})
    require(code==200 and instrument['available'] and len(instrument['observedFunctions'])==26,'Real function capability missing')
    probe=subprocess.check_output([sys.executable,'-m','tools.observability.probe','trace','--run-dir',str(run)],text=True,timeout=20)
    probe=json.loads(probe);code,trace=request(base,'/api/v1/traces/'+probe['traceId'])
    require(code==200 and trace['status']=='complete' and len({row['service'] for row in trace['spans']})>=2,'Cross-process trace incomplete')
    outage=None;pid=processes['prometheus']
    if args.exercise_outage:
        os.kill(pid,signal.SIGSTOP)
        try:
            status,failed=request(base,'/api/v1/series',{'from':now-900,'to':now,'panel':'function_send_p99'})
            require(status==503 and failed['status']=='unavailable','Metrics failure was disguised as an empty result')
            require(request(base,'/health')[0]==200,'Telemetry outage stopped Observer')
            require(request(base,'/api/v1/traces/'+probe['traceId'])[0]==200,'Telemetry outage blocked stored traces')
            outage={'unavailableHttp':status,'observerStayedAlive':True,'storedTraceReadable':True}
        finally:os.kill(pid,signal.SIGCONT)
        code,recovered=request(base,'/api/v1/series',{'from':now-900,'to':int(time.time()),'panel':'function_send_p99'})
        require(code==200 and recovered['status']=='ok','Metrics did not recover')
        outage['recoveredHttp']=code
    installed=Path('/home/pikachu/phase11-deps/observability/metrics-install-manifest.json')
    report={'passed':True,'simulatedBackend':False,'businessFixture':'synthetic-disposable-database',
        'opsAccess':access,'source':'Prometheus','targets':[{'labels':row['labels'],'health':row['health']} for row in targets],
        'p99Seconds':values,'expression':expression,'functions':len(instrument['observedFunctions']),
        'crossProcess':{'traceId':probe['traceId'],'spanCount':len(trace['spans']),
                        'services':sorted({row['service'] for row in trace['spans']})},
        'outage':outage,'backendManifestSha256':hashlib.sha256(installed.read_bytes()).hexdigest(),
        'retentionDays':7,'dailyServiceChanged':False,'secretsIncluded':False}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':True,'realTargets':len(targets),'functions':26,'outage':outage,'output':str(args.output)}))


if __name__=='__main__':main()
