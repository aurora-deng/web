"""Three interleaved equivalent-config pre-Phase43/current runs; owned test port and databases only."""
import json
import hashlib
import argparse
import os
from pathlib import Path
import re
import signal
import socket
import statistics
import subprocess
import shutil
import time

SOURCE=Path('/home/pikachu/function3-src');BASE=SOURCE
ROOT=Path('/home/pikachu/phase11-data/function3/validation')
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--diagnostic',action='store_true');parser.add_argument('--synchronous-commit',choices=('on','off'),default='on');parser.add_argument('--label',default='',help='Separate evidence directory for a later explicitly justified rerun')
parser.add_argument('--stage',choices=('phase43-47','phase48'),default='phase43-47')
parser.add_argument('--baseline-stage',choices=('phase43-47','phase48'),default='phase43-47')
a=parser.parse_args()
BEFORE=ROOT/a.baseline_stage/'webserver-before'
if a.label and not re.fullmatch(r'[a-z0-9-]{1,40}',a.label):parser.error('label must be a short lowercase directory name')
if a.synchronous_commit=='off' and not a.diagnostic:parser.error('Asynchronous commit is a non-durable diagnostic control only, never an acceptance configuration')
OUT=ROOT/a.stage/('diagnostic-'+a.synchronous_commit if a.diagnostic else 'benchmarks')
if a.label:OUT=OUT.with_name(OUT.name+'-'+a.label)
if (OUT/'summary.json').exists():raise RuntimeError('Evidence already exists; preserve it and choose a new --label for a justified rerun')
OUT.mkdir(parents=True,exist_ok=True)
if not BEFORE.is_file():raise RuntimeError('Save the exact pre-change executable first; no substitute baseline accepted')
subprocess.run(['python3',str(SOURCE/'scripts/function3/validation/run_server.py'),'stop'],check=True)
summary={'stage':a.stage,'baselineStage':a.baseline_stage,'baselineBinarySha256':hashlib.sha256(BEFORE.read_bytes()).hexdigest(),'currentBinarySha256':hashlib.sha256((SOURCE/'build-release/webserver').read_bytes()).hexdigest(),'configuration':{'reactors':2,'httpWorkers':4,'databaseWorkers':4,'databasePool':4,'delivery':'local-durable' if a.synchronous_commit=='on' else 'non-durable-diagnostic-only','redis':False,'tts':False,'ai':False,'tlsPort':18444,'diagnostic':a.diagnostic,'synchronousCommit':a.synchronous_commit},'baseline':[],'current':[]}
# 复用已存在的基准库，追加缺少的迁移；不创建数据库、不接触原业务库。
pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
baseline_connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_baseline_test'
assert subprocess.check_output([pg,baseline_connection,'-w','-qAtc','SELECT current_database()'],text=True).strip()=='function3_baseline_test'
applied=set(subprocess.check_output([pg,baseline_connection,'-w','-qAtc','SELECT version FROM phase11_schema_migrations'],text=True).splitlines())
for migration in sorted((SOURCE/'backend/application/storage/postgres/migrations').glob('*.sql')):
    if str(int(migration.name.split('_',1)[0])) not in applied:
        subprocess.run([pg,baseline_connection,'-w','-v','ON_ERROR_STOP=1','-f',str(migration)],check=True,stdout=subprocess.DEVNULL)
rounds=1 if a.diagnostic else 3
for round_number in range(1,rounds+1):
    for variant in (('baseline','current') if round_number%2 else ('current','baseline')):
        folder=BASE if variant=='baseline' else SOURCE;db='function3_baseline_test' if variant=='baseline' else 'function3_phase11_test'
        # 两个库本来就是可重置的基准/契约库。每轮清空测试夹具并重置ID，避免不同历史行数
        # 混入架构对比；真实HTTP夹具库 runtime_test 和原业务 phase11 均不进入此路径。
        assert db in ('function3_baseline_test','function3_phase11_test')
        pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql';connection=f'host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname={db}'
        assert subprocess.check_output([pg,connection,'-w','-qAtc','SELECT current_database()'],text=True).strip()==db
        reset="DO $$DECLARE targets text; BEGIN SELECT string_agg(format('%I.%I',schemaname,tablename),',') INTO targets FROM pg_tables WHERE schemaname='public' AND tablename NOT IN ('phase11_schema_migrations','tree_categories','speech_profiles'); EXECUTE 'TRUNCATE '||targets||' RESTART IDENTITY CASCADE'; END$$;"
        subprocess.run([pg,connection,'-w','-v','ON_ERROR_STOP=1','-qc',reset],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        env={key:value for key,value in os.environ.items() if not key.startswith(('WEB_','ASAN_','UBSAN_'))}
        env.update({'WEB_SERVER_PORT':'18094','WEB_SERVER_REACTORS':'2','WEB_HTTP_WORKERS':'4','WEB_WS_WORKERS':'4','WEB_GRPC_ADDRESS':'127.0.0.1:50064',
                    'WEB_PHASE11_DATABASE':f'host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname={db}',
                    'WEB_PHASE11_DB_WORKERS':'4','WEB_PHASE11_DB_POOL':'4','WEB_FUNCTION3_ENABLE':'1',
                    'WEB_PHASE11_TOKEN_KEY_FILE':str(ROOT/'current.token.key'),
                    'WEB_PHASE11_FILE_ROOT':str(OUT/(variant+'-files')),'WEB_FUNCTION3_MEDIA_ROOT':str(OUT/(variant+'-media')),
                    'WEB_TLS_CERT':'/home/pikachu/phase11-data/function3/runtime/localhost.crt','WEB_TLS_KEY':'/home/pikachu/phase11-data/function3/runtime/localhost.key','WEB_TLS_PORT':'18444'})
        if a.diagnostic:env['WEB_PHASE11_DATABASE']+=" options='-c track_wal_io_timing=on -c track_io_timing=on -c synchronous_commit="+a.synchronous_commit+"'"
        for port in (18094,18444,50064):
            with socket.socket() as probe:
                probe.settimeout(.2)
                if probe.connect_ex(('127.0.0.1',port))==0:raise RuntimeError(f'Test port {port} already has a listener; refusing to benchmark another process')
        binary=BEFORE if variant=='baseline' else SOURCE/'build-release/webserver'
        assert hashlib.sha256(binary.read_bytes()).hexdigest()==summary['baselineBinarySha256' if variant=='baseline' else 'currentBinarySha256'],'Binary changed during measurement'
        log=(OUT/f'{variant}-{round_number}-server.log').open('wb')
        working=OUT/(variant+'-work');working.mkdir(exist_ok=True)
        shutil.copytree(SOURCE/'frontend/phase11',working/'frontend/phase11',dirs_exist_ok=True)
        shutil.copytree(SOURCE/'static',working/'static',dirs_exist_ok=True)
        process=subprocess.Popen([str(binary)],cwd=working,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        ticks=(Path('/proc')/str(process.pid)/'stat').read_text().split()[21]
        try:
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                if process.poll() is not None:raise RuntimeError('Benchmark server exited')
                if subprocess.run(['curl','-fsS','--max-time','1','http://127.0.0.1:18094/health/live'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:break
                time.sleep(.1)
            else:raise RuntimeError('Benchmark server not live')
            time.sleep(3)
            if process.poll() is not None:raise RuntimeError('Benchmark server exited after readiness')
            subprocess.run(['wrk','-t2','-c64','-d3s','http://127.0.0.1:18094/fast'],stdout=subprocess.DEVNULL,check=True)
            fast=subprocess.check_output(['wrk','-t2','-c64','-d10s','--latency','http://127.0.0.1:18094/fast'],text=True)
            (OUT/f'{variant}-{round_number}-fast.txt').write_text(fast)
            assert not re.search(r'(Socket errors|Non-2xx or 3xx responses)',fast),fast
            destination=OUT/f'{variant}-{round_number}-mixed.json'
            command=['python3',str(SOURCE/'benchmarks/function3_mixed.py'),'--server-pid',str(process.pid),'--output',str(destination)]
            if a.diagnostic:command+=['--observe-database',db]
            subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
            mixed=json.loads(destination.read_text());entry={'round':round_number,'pid':process.pid,'fastQps':float(re.search(r'Requests/sec:\s+([\d.]+)',fast)[1]),'ackP99Ms':mixed['durableChatAckMs']['p99'],'errors':mixed['mixed']['errors']}
            tree_output=OUT/f'{variant}-{round_number}-tree.json'
            subprocess.run(['python3',str(SOURCE/'benchmarks/function3_tree_reads.py'),'--output',str(tree_output)],check=True,stdout=subprocess.DEVNULL)
            entry['treeReadLatencyMs']=json.loads(tree_output.read_text())['latencyMs']
            summary[variant].append(entry);(OUT/'progress.json').write_text(json.dumps(summary,indent=2)+'\n');print(variant,entry,flush=True)
        finally:
            # Popen and start ticks identify this exact child; never terminate another listener.
            if process.poll() is None:
                assert (Path('/proc')/str(process.pid)/'stat').read_text().split()[21]==ticks
                os.kill(process.pid,signal.SIGTERM);process.wait(timeout=45)
            log.close()
summary['fastRegressionPercent']=100*(1-statistics.median(x['fastQps'] for x in summary['current'])/statistics.median(x['fastQps'] for x in summary['baseline']))
summary['pairedAckP99IncreasePercent']=[100*(next(x for x in summary['current'] if x['round']==i)['ackP99Ms']/next(x for x in summary['baseline'] if x['round']==i)['ackP99Ms']-1) for i in range(1,rounds+1)]
summary['gateEvaluated']=not a.diagnostic
summary['passed']=not a.diagnostic and summary['fastRegressionPercent']<=10 and all(x<=20 for x in summary['pairedAckP99IncreasePercent']) and all(x['errors']==0 for key in ('current','baseline') for x in summary[key])
(OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
if not a.diagnostic and not summary['passed']:raise SystemExit('Phase47 performance gate not met; retain measurements and previous Phase34/41 results')
