"""Three paired rounds with the same pooled upstream TLS policy; existing binaries only."""
import argparse
import json
from pathlib import Path
import queue
import statistics
import subprocess
import threading
import time
from native_integration import Browser,root
from measure_native import NativeMemory

p=argparse.ArgumentParser();p.add_argument('--certificate-sha256',required=True);p.add_argument('--label',default='');a=p.parse_args()
if a.label and (not a.label.isascii() or not a.label.replace('-','').isalnum()):p.error('label must contain ASCII letters/digits/hyphens')
out=root/('build-client/phase43-47-proxy'+('-'+a.label if a.label else ''));out.mkdir(parents=True,exist_ok=True)
if (out/'summary.json').exists():raise RuntimeError('Preserve existing evidence; choose a new --label for a justified rerun')
private=out/'private';private.mkdir(exist_ok=True);marker=private/'NEBULA_PRIVATE_ROOT'
if not marker.exists():assert not list(private.iterdir());marker.write_bytes(b'nebula-private-root-v1\n')
stop=out/'stop';stop.unlink(missing_ok=True)
command=[str(root/'build-client/release/nebula_companion.exe'),'--server','192.168.239.135','--tls-port','18444','--connect-host','127.0.0.1','--certificate-sha256',a.certificate_sha256,'--frontend',str(root/'frontend/phase11'),'--data-dir',str(private),'--stop-file',str(stop)]
process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',creationflags=subprocess.CREATE_NO_WINDOW)
memory=NativeMemory(process.pid);lines=queue.Queue();report={'passed':False,'policy':'both paths use verified pooled TLS; each round excludes warmup','rounds':[]}
def reader():
    for line in process.stdout:lines.put(line)
threading.Thread(target=reader,daemon=True).start()
try:
    url='';deadline=time.monotonic()+25
    while time.monotonic()<deadline:
        try:line=lines.get(timeout=1)
        except queue.Empty:
            if process.poll() is not None:raise RuntimeError('client exited')
            continue
        if line.startswith('http://localhost:'):url=line.strip();break
    assert url,'bootstrap unavailable'
    browser=Browser(18091);browser.call('POST','/api/local/session/bootstrap',{'token':url.split('companion=')[1]})
    for number in range(1,4):
        values={}
        for variant in (('direct','proxy') if number%2 else ('proxy','direct')):
            if variant=='direct':
                result=subprocess.run([str(root/'build-client/release/companion_tls_probe.exe'),'192.168.239.135','18444',a.certificate_sha256,'127.0.0.1','--measure-reuse'],capture_output=True,text=True,check=True,timeout=120)
                values[variant]=json.loads(result.stdout)
            else:
                assert browser.call('GET','/health/live')['status']=='alive';samples=[]
                for _ in range(30):
                    start=time.perf_counter();assert browser.call('GET','/health/live')['status']=='alive';samples.append((time.perf_counter()-start)*1000)
                samples.sort();values[variant]={'samples':30,'p50Ms':round(statistics.median(samples),3),'p95Ms':round(samples[28],3)}
        report['rounds'].append({'round':number,**values,'p50AddedMs':round(values['proxy']['p50Ms']-values['direct']['p50Ms'],3),'errors':0})
    report['pool']=browser.call('GET','/api/local/session/info')['httpPool']
    # Transport attempt failures include safely recovered stale keep-alive GETs.
    # Every measured operation above still has to return HTTP 200; never equate a
    # successful read retry with a lost business operation or silently replay writes.
    report['recoveredReadAttempts']=report['pool']['readRetries']
    assert report['pool']['failures']==report['recoveredReadAttempts']
    report['passed']=True;print(json.dumps(report,indent=2))
finally:
    report['resources']=memory.stop();stop.touch()
    try:process.wait(timeout=45)
    except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10);report['passed']=False
    report['gracefulShutdown']=process.returncode==0
    (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
if not report['passed'] or not report['gracefulShutdown']:raise SystemExit('proxy comparison failed')
