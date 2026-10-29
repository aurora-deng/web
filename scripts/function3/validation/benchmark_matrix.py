"""Three rounds against the two isolated builds. Does not control formal services."""
import json
from pathlib import Path
import re
import statistics
import subprocess
import time

source=Path('/home/pikachu/function3-src')
controller=source/'scripts/function3/validation/run_server.py'
results=Path('/home/pikachu/phase11-data/function3/validation/benchmarks');results.mkdir(parents=True,exist_ok=True)
ns=Path('/home/pikachu/phase11-data/function3/rocketmq/namespace.pid').read_text().strip()
network=['nsenter','--preserve-credentials','-t',ns,'-U','-n']
summary={variant:[] for variant in ('baseline','current')}
for round in range(1,4):
    for variant in (('baseline','current') if round%2 else ('current','baseline')):
        for name in ('current','baseline'):subprocess.run(['python3',str(controller),'stop','--variant',name],check=True)
        subprocess.run(['python3',str(controller),'start','--variant',variant,'--with-mq','--tts'],check=True)
        pid=int((results.parent/(variant+'.pid')).read_text())
        if variant=='current':
            # /fast 对比不与启动时遗留的 TTS 生成争抢 CPU；混合指标另行记录。
            query="SELECT count(*) FROM announcement_audio WHERE state='pending' AND expires_ms>extract(epoch FROM CURRENT_TIMESTAMP)*1000"
            pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
            connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_runtime_test'
            deadline=time.monotonic()+120
            while subprocess.check_output([pg,connection,'-w','-qAtc',query],text=True).strip()!='0':
                if time.monotonic()>deadline:raise RuntimeError('startup TTS did not become idle; benchmark refused')
                time.sleep(.5)
        subprocess.run(network+['wrk','-t2','-c64','-d3s','http://127.0.0.1:18094/fast'],stdout=subprocess.DEVNULL,check=True)
        fast=subprocess.check_output(network+['wrk','-t2','-c64','-d10s','--latency','http://127.0.0.1:18094/fast'],text=True)
        (results/f'{variant}-{round}-fast.txt').write_text(fast)
        assert not re.search(r'(Socket errors|Non-2xx or 3xx responses)',fast),fast
        qps=float(re.search(r'Requests/sec:\s+([\d.]+)',fast)[1])
        destination=results/f'{variant}-{round}-mixed.json'
        subprocess.run(network+['python3',str(source/'benchmarks/function3_mixed.py'),'--server-pid',str(pid),'--output',str(destination)],check=True)
        mixed=json.loads(destination.read_text());summary[variant].append({'round':round,'pid':pid,'fastQps':qps,'ackP99Ms':mixed['durableChatAckMs']['p99'],'mixedErrors':mixed['mixed']['errors']})
        (results/'progress.json').write_text(json.dumps(summary,indent=2))
        print(variant,round,'QPS',qps,'ACK P99',mixed['durableChatAckMs']['p99'],flush=True)
        subprocess.run(['python3',str(controller),'stop','--variant',variant],check=True)
for variant,items in summary.items():
    summary[variant]={'runs':items,'medianFastQps':statistics.median(x['fastQps'] for x in items),'medianAckP99Ms':statistics.median(x['ackP99Ms'] for x in items)}
summary['fastRegressionPercent']=100*(1-summary['current']['medianFastQps']/summary['baseline']['medianFastQps'])
summary['ackP99IncreasePercent']=100*(summary['current']['medianAckP99Ms']/summary['baseline']['medianAckP99Ms']-1)
summary['gatesPassed']=summary['fastRegressionPercent']<=10 and summary['ackP99IncreasePercent']<=20
(results/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2),flush=True)
if not summary['gatesPassed']:raise SystemExit('Performance gate failed; investigate the saved equivalent-configuration runs.')
