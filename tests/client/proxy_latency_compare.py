"""Three interleaved real native -> approved TLS comparisons, no daily service.

The before executable must already have been preserved. A missing baseline is an
error, never silently substituted by the current binary. Uses existing tools.
"""
from pathlib import Path
import argparse
import hashlib
import json
import queue
import statistics
import subprocess
import threading
import time
from native_integration import Browser, root
from measure_native import NativeMemory

p=argparse.ArgumentParser();p.add_argument('--certificate-sha256',required=True);a=p.parse_args()
output=root/'build-client/phase42-latency';output.mkdir(parents=True,exist_ok=True)
executables={'before':root/'build-client/phase42-baseline/nebula_companion.exe','after':root/'build-client/release/nebula_companion.exe'}
report={'transport':'localhost HTTP -> native Schannel TLS1.3 via SSH -> isolated Linux 18444','samplesPerRound':30,'results':[],'binarySha256':{k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in executables.items()}}
try:
    for number in range(1,4):
        for variant in (('before','after') if number%2 else ('after','before')):
            directory=output/variant;directory.mkdir(exist_ok=True);private=directory/'private';private.mkdir(exist_ok=True)
            marker=private/'NEBULA_PRIVATE_ROOT'
            if not marker.exists():assert not list(private.iterdir());marker.write_bytes(b'nebula-private-root-v1\n')
            stop=directory/'stop';stop.unlink(missing_ok=True)
            command=[str(executables[variant]),'--server','192.168.239.135','--tls-port','18444','--connect-host','127.0.0.1','--certificate-sha256',a.certificate_sha256,'--frontend',str(root/'frontend/phase11'),'--data-dir',str(private),'--stop-file',str(stop)]
            process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',creationflags=subprocess.CREATE_NO_WINDOW)
            memory=NativeMemory(process.pid);lines=queue.Queue()
            def read():
                for line in process.stdout:lines.put(line)
            threading.Thread(target=read,daemon=True).start()
            try:
                url='';deadline=time.monotonic()+25
                while time.monotonic()<deadline:
                    try:line=lines.get(timeout=1)
                    except queue.Empty:
                        if process.poll() is not None:raise RuntimeError('client exited')
                        continue
                    if line.startswith('http://localhost:'):url=line.strip();break
                assert url,'bootstrap missing'
                browser=Browser(18091);browser.call('POST','/api/local/session/bootstrap',{'token':url.split('companion=')[1]})
                started=time.perf_counter();assert browser.call('GET','/health/live')['status']=='alive';cold=(time.perf_counter()-started)*1000
                values=[]
                for _ in range(30):
                    started=time.perf_counter();assert browser.call('GET','/health/live')['status']=='alive';values.append((time.perf_counter()-started)*1000)
                stats=browser.call('GET','/api/local/session/info').get('httpPool')
                if variant=='after':assert stats and stats['connected']==1 and stats['reused']>=30 and stats['failures']==0,stats
                report['results'].append({'round':number,'variant':variant,'coldMs':round(cold,3),'p50Ms':round(statistics.median(values),3),'p95Ms':round(sorted(values)[28],3),'errors':0,'httpPool':stats,'resources':memory.stop()})
                print(variant,number,round(statistics.median(values),3),'ms',flush=True)
            finally:
                memory.stop();stop.touch()
                try:process.wait(timeout=45)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10);raise RuntimeError('owned native client failed to stop')
                assert process.returncode==0,'native client exit failed'
    report['passed']=True
finally:(output/'summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
