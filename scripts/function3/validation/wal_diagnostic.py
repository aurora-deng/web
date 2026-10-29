"""Compare private PostgreSQL WAL and Broker I/O for the same fixed mixed workload.

Only the marked test cluster's timing settings are changed and restored. No formal
database/service, OS tuning, package, download, or durability setting is changed.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--wait-pid',type=int,required=True);a=p.parse_args()
source=Path('/home/pikachu/function3-src');root=Path('/home/pikachu/phase11-data/function3/validation')
assert (root.parent/'pg/FUNCTION3_TEST_CLUSTER').exists()
process=Path('/proc')/str(a.wait_pid)
while process.exists() and (process/'stat').read_text().split()[2]!='Z':time.sleep(1)
controller=source/'scripts/function3/validation/run_server.py'
pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_runtime_test'
def sql(query):return subprocess.check_output([pg,connection,'-w','-qAtc',query],text=True).strip()
def run(args,**kwargs):return subprocess.run([str(x) for x in args],check=True,cwd=source,**kwargs)
settings={name:sql('SHOW '+name) for name in ('track_io_timing','track_wal_io_timing')}
report={'settingsBefore':settings,'runs':{}}
def io():
    rows=json.loads(sql("SELECT json_agg(t) FROM pg_stat_io t WHERE object='wal'"))
    result={field:sum((row.get(field) or 0) for row in rows) for field in ('writes','write_bytes','write_time','fsyncs','fsync_time')}
    broker=int((root.parent/'rocketmq/broker.pid').read_text());result['brokerIo']={k:int(v) for k,v in (line.split(':') for line in (Path('/proc')/str(broker)/'io').read_text().splitlines())}
    return result
try:
    for name in settings:sql('ALTER SYSTEM SET '+name+"='on'")
    sql('SELECT pg_reload_conf()')
    run(['python3',source/'scripts/function3/dependencies/isolated_rocketmq.py','start'])
    ns=(root.parent/'rocketmq/namespace.pid').read_text().strip();network=['nsenter','--preserve-credentials','-t',ns,'-U','-n']
    time.sleep(5)
    for variant in ('baseline','current'):
        run(['python3',controller,'start','--variant',variant,'--with-mq','--tts'])
        pid=(root/(variant+'.pid')).read_text().strip();before=io();output=root/(variant+'-wal-mixed.json')
        with (root/(variant+'-wal-mixed.txt')).open('w') as log:
            run(network+['python3',source/'benchmarks/function3_mixed.py','--server-pid',pid,'--output',output],stdout=log,stderr=subprocess.STDOUT)
        time.sleep(1);after=io();metric=json.loads(output.read_text())
        report['runs'][variant]={'ackP99Ms':metric['durableChatAckMs']['p99'],'errors':metric['mixed']['errors'],
            'walDelta':{k:round(after[k]-before[k],3) for k in before if k!='brokerIo'},
            'brokerIoDelta':{k:after['brokerIo'][k]-before['brokerIo'][k] for k in before['brokerIo']}}
        print(variant,json.dumps(report['runs'][variant]),flush=True)
        run(['python3',controller,'stop','--variant',variant])
finally:
    try:
        for variant in ('baseline','current'):run(['python3',controller,'stop','--variant',variant])
        run(['python3',source/'scripts/function3/dependencies/isolated_rocketmq.py','stop'])
    finally:
        # 即使进程收尾失败，也必须保存原始结果并恢复本测试自己的计时配置。
        for name,value in settings.items():sql('ALTER SYSTEM SET '+name+"='"+value+"'")
        sql('SELECT pg_reload_conf()')
        (root/'wal-diagnostic.json').write_text(json.dumps(report,indent=2)+'\n')
