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
import datetime
from measurement_noise import digest, analyse, acceptance_result

SOURCE=Path('/home/pikachu/function3-src');BASE=SOURCE
ROOT=Path('/home/pikachu/phase11-data/function3/validation')
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--diagnostic',action='store_true');parser.add_argument('--synchronous-commit',choices=('on','off'),default='on');parser.add_argument('--label',default='',help='Separate evidence directory for a later explicitly justified rerun')
parser.add_argument('--stage',choices=('phase43-47','phase48','phase49','phase50'),default='phase43-47')
parser.add_argument('--baseline-stage',choices=('phase43-47','phase48','phase49','phase50'),default='phase43-47')
parser.add_argument('--rounds',type=int,default=3,help='Interleaved rounds per variant. Use >=10 for A/A noise-floor runs; the acceptance gate keeps 3.')
parser.add_argument('--baseline-binary',default=None,help='Explicit baseline executable path. Pass the current build to run an A/A noise-floor test without touching the archived baseline under <baseline-stage>/webserver-before.')
parser.add_argument('--current-binary',type=Path,default=SOURCE/'build-release/webserver',help='Explicit current binary; never overwritten by this script')
parser.add_argument('--calibration',action='store_true',help='Explicit same-binary A/A mode; labels never change acceptance policy')
parser.add_argument('--operations',type=int,default=1200,help='Mixed operations per variant; calibration requires >=4000')
a=parser.parse_args()
BEFORE=Path(a.baseline_binary).resolve() if a.baseline_binary else ROOT/a.baseline_stage/'webserver-before'
CURRENT=a.current_binary.resolve()
if a.label and not re.fullmatch(r'[a-z0-9-]{1,40}',a.label):parser.error('label must be a short lowercase directory name')
if a.synchronous_commit=='off' and not a.diagnostic:parser.error('Asynchronous commit is a non-durable diagnostic control only, never an acceptance configuration')
if a.rounds<1 or a.operations<10:parser.error('positive rounds and >=10 operations required')
if a.calibration and (a.diagnostic or a.rounds<10 or a.operations<4000):parser.error('Calibration needs >=10 rounds, >=4000 operations and durable commit')
if not a.calibration and not a.diagnostic and a.rounds<3:parser.error('Acceptance needs >=3 complete rounds')
if not BEFORE.is_file() or not CURRENT.is_file():parser.error('Both exact executables must exist; this script never builds them')
binary_hashes={name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in [('baseline',BEFORE),('current',CURRENT)]}
if a.calibration and binary_hashes['baseline']!=binary_hashes['current']:parser.error('Calibration requires identical executable SHA-256')
# Serialize all stages on these fixed test ports. Never stop an existing service.
import fcntl
ROOT.mkdir(parents=True,exist_ok=True)
lock=(ROOT/'benchmark.lock').open('a')
fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
for port in (18094,18444,50064):
    with socket.socket() as probe:
        probe.settimeout(.2)
        if probe.connect_ex(('127.0.0.1',port))==0:raise RuntimeError(f'Test port {port} occupied; no service was stopped or database reset')
OUT=ROOT/a.stage/('diagnostic-'+a.synchronous_commit if a.diagnostic else 'benchmarks')
if a.label:OUT=OUT.with_name(OUT.name+'-'+a.label)
if (OUT/'summary.json').exists() or (OUT/'progress.json').exists():raise RuntimeError('Evidence already exists; preserve it and choose a new --label for a justified rerun')
OUT.mkdir(parents=True,exist_ok=True)
if not BEFORE.is_file():raise RuntimeError('Save the exact pre-change executable first; no substitute baseline accepted')
summary={'stage':a.stage,'baselineStage':a.baseline_stage,'mode':'calibration' if a.calibration else 'diagnostic' if a.diagnostic else 'acceptance','roundsRequested':a.rounds,'baselineBinarySha256':binary_hashes['baseline'],'currentBinarySha256':binary_hashes['current'],'configuration':{'reactors':2,'httpWorkers':4,'wsWorkers':4,'databaseWorkers':4,'databasePool':4,'delivery':'local-durable' if a.synchronous_commit=='on' else 'non-durable-diagnostic-only','redis':False,'tts':False,'ai':False,'tlsPort':18444,'diagnostic':a.diagnostic,'synchronousCommit':a.synchronous_commit},'baseline':[],'current':[]}
summary['workload']={'fastWarmupSeconds':3,'fastSeconds':10,'fastThreads':2,'fastConnections':64,'mixedOperations':a.operations,'mixedWorkers':4,'httpListPercent':50,'historyPercent':20,'chatPercent':30,'residentSse':4,'harnessSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'mixedSourceSha256':hashlib.sha256((SOURCE/'benchmarks/function3_mixed.py').read_bytes()).hexdigest(),'treeSourceSha256':hashlib.sha256((SOURCE/'benchmarks/function3_tree_reads.py').read_bytes()).hexdigest(),'supportSourceSha256':hashlib.sha256((SOURCE/'benchmarks/phase15_business_benchmark.py').read_bytes()).hexdigest()}
summary['environment']={'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'kernel':os.uname().release,'cpuCount':os.cpu_count(),'loadAverageStart':os.getloadavg(),'governor':Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor').read_text().strip() if Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor').is_file() else 'unavailable','globalSettingsModified':False}
# 复用已存在的基准库，追加缺少的迁移；不创建数据库、不接触原业务库。
pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
baseline_connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_baseline_test'
assert subprocess.check_output([pg,baseline_connection,'-w','-qAtc','SELECT current_database()'],text=True).strip()=='function3_baseline_test'
applied=set(subprocess.check_output([pg,baseline_connection,'-w','-qAtc','SELECT version FROM phase11_schema_migrations'],text=True).splitlines())
for migration in sorted((SOURCE/'backend/application/storage/postgres/migrations').glob('*.sql')):
    if str(int(migration.name.split('_',1)[0])) not in applied:
        subprocess.run([pg,baseline_connection,'-w','-v','ON_ERROR_STOP=1','-f',str(migration)],check=True,stdout=subprocess.DEVNULL)
rounds=1 if a.diagnostic else a.rounds
if rounds<1:parser.error('--rounds must be >= 1')
for round_number in range(1,rounds+1):
    for variant in (('baseline','current') if round_number%2 else ('current','baseline')):
        folder=BASE if variant=='baseline' else SOURCE;db='function3_baseline_test' if a.calibration or variant=='baseline' else 'function3_phase11_test'
        # 两个库本来就是可重置的基准/契约库。每轮清空测试夹具并重置ID，避免不同历史行数
        # 混入架构对比；真实HTTP夹具库 runtime_test 和原业务 phase11 均不进入此路径。
        assert db in ('function3_baseline_test','function3_phase11_test')
        pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql';connection=f'host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname={db}'
        assert subprocess.check_output([pg,connection,'-w','-qAtc','SELECT current_database()'],text=True).strip()==db
        reset="DO $$DECLARE targets text; BEGIN SELECT string_agg(format('%I.%I',schemaname,tablename),',') INTO targets FROM pg_tables WHERE schemaname='public' AND tablename NOT IN ('phase11_schema_migrations','tree_categories','speech_profiles'); EXECUTE 'TRUNCATE '||targets||' RESTART IDENTITY CASCADE'; END$$;"
        subprocess.run([pg,connection,'-w','-v','ON_ERROR_STOP=1','-qc',reset],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        # Only metadata/seed digests enter the report. A/A reuses the same resettable
        # DB on both sides so different preserved categories/profiles cannot bias it.
        fixture=[]
        for table in ('phase11_schema_migrations','tree_categories','speech_profiles'):
            fixture.append(subprocess.check_output([pg,connection,'-w','-qAtc',f'SELECT row_to_json(t)::text FROM public.{table} t ORDER BY row_to_json(t)::text'],text=True))
        fixture.append(subprocess.check_output([pg,connection,'-w','-qAtc',"SELECT table_name,column_name,data_type,is_nullable,column_default FROM information_schema.columns WHERE table_schema='public' ORDER BY table_name,ordinal_position"],text=True))
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
        binary=BEFORE if variant=='baseline' else CURRENT
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
            command=['python3',str(SOURCE/'benchmarks/function3_mixed.py'),'--server-pid',str(process.pid),'--operations',str(a.operations),'--output',str(destination)]
            if a.diagnostic:command+=['--observe-database',db]
            subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
            mixed=json.loads(destination.read_text());entry={'round':round_number,'pid':process.pid,'fastQps':float(re.search(r'Requests/sec:\s+([\d.]+)',fast)[1]),'ackP99Ms':mixed['durableChatAckMs']['p99'],'errors':mixed['mixed']['errors']}
            entry.update(startTicks=ticks,database=db,binarySha256=binary_hashes[variant],configurationSha256=digest(summary['configuration']),workloadSha256=digest(summary['workload']),fixtureSha256=digest(fixture),ackSamples=mixed['durableChatAckSamples'])
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
summary['gateEvaluated'],summary['passed']=acceptance_result(summary)
summary['environment']['loadAverageEnd']=os.getloadavg()
if a.calibration:
    summary['calibrationCompleted']=True
    summary['calibrationDescription']=analyse(summary)
(OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
if summary['gateEvaluated'] and not summary['passed']:raise SystemExit('Performance gate not met; retain measurements and previous Phase34/41 results')
