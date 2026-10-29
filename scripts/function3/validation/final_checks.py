"""Final isolated checks after an existing build job; never touches formal services.

Uses only installed tools, the marked test cluster and retained model. Individual
failures stop dependent checks; performance gate failures remain visible while AI
integration can still run. No download, commit, push or automatic formal deployment.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--wait-pid',type=int,required=True)
p.add_argument('--integration-only',action='store_true')
p.add_argument('--reuse-release',action='store_true',help='Reuse the verified binary after removing only regenerable objects')
p.add_argument('--history-tag',default='before-delivery-isolation')
a=p.parse_args();source=Path('/home/pikachu/function3-src')
assert re.fullmatch(r'[a-z0-9-]{1,40}',a.history_tag)
root=Path('/home/pikachu/phase11-data/function3/validation')
assert (root.parent/'pg/FUNCTION3_TEST_CLUSTER').exists()
def run(args,**kwargs):return subprocess.run([str(x) for x in args],check=True,cwd=source,**kwargs)
while (process:=Path('/proc')/str(a.wait_pid)).exists() and (process/'stat').read_text().split()[2]!='Z':time.sleep(1)
assert json.loads((root/'checks/asan/summary.json').read_text())['passed'],'repair sanitizer failure first'
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
# Last revocation change happened during the first large build. Rebuild its core
# dependencies and execute all gtests in both configurations instead of claiming
# that the earlier binaries already contained that change.
if not a.integration_only:
    for flavour in ('debug','asan'):
        run(['cmake','--build',source/('build-'+flavour),'--target','webserver_unit_tests','-j','2'])
        with (root/(flavour+'-final-core.log')).open('w') as log:
            run([source/('build-'+flavour)/'webserver_unit_tests'],env=env if flavour=='asan' else None,stdout=log,stderr=subprocess.STDOUT)
        print(flavour,'final core regression passed',flush=True)
env['PHASE11_POSTGRES_CONNECTION']='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test'
if not a.reuse_release:
    run(['cmake','-S',source,'-B',source/'build-release'])
    run(['cmake','--build',source/'build-release','-j','2'])
    env['PHASE11_POSTGRES_CONNECTION']='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test'
    with (root/'release-final-ctest.log').open('w') as log:
        run(['ctest','--test-dir',source/'build-release','--output-on-failure','-j','2'],env=env,stdout=log,stderr=subprocess.STDOUT)
    with (root/'capacity-final.json').open('w') as log:
        run([source/'build-release/backend/application/modules/function3_capacity','--isolated-test-only'],env=env,stdout=log)
else:
    assert re.search(r'100% tests passed, 0 tests failed out of [1-9][0-9]*',(root/'release-final-ctest.log').read_text())
    checked=(root/'release-final-ctest.log').stat().st_mtime
    compiled=[source/'main.cpp',source/'CMakeLists.txt']
    for folder in ('server','frontend'):
        compiled += [f for f in (source/folder).rglob('*') if f.is_file() and f.suffix in ('.h','.cpp','.js','.html','.css')]
    assert all(f.stat().st_mtime<=checked for f in compiled),'source changed after recorded regression; rebuild instead of reusing'
keeper=root.parent/'rocketmq/namespace.pid'
existing=Path('/proc')/keeper.read_text().strip() if keeper.exists() else Path('/proc/0')
if existing.exists() and (existing/'stat').read_text().split()[2]!='Z':
    assert str(root.parent/'rocketmq') in (existing/'cmdline').read_bytes().decode()
    print('Reusing the already-running marked private Broker',flush=True)
else:run(['python3',source/'scripts/function3/dependencies/isolated_rocketmq.py','start'])
ns=keeper.read_text().strip()
network=['nsenter','--preserve-credentials','-t',ns,'-U','-n']
probe="import socket,time\nfor i in range(100):\n try:\n  s=socket.create_connection(('127.0.0.1',20881),.5);s.close();break\n except OSError:time.sleep(.2)\nelse:raise SystemExit('private Proxy did not start')"
run(network+['python3','-c',probe])
run(['python3',source/'scripts/function3/dependencies/prepare_rocketmq_topics.py'])
with (root/'mq-final.log').open('w') as log:
    run(network+[source/'build-release/backend/application/modules/function3_rocketmq_roundtrip'],stdout=log,stderr=subprocess.STDOUT)
controller=source/'scripts/function3/validation/run_server.py'
try:
    run(['python3',controller,'start','--with-mq','--tts'])
    with (root/'business-final.log').open('w') as log:
        run(network+['python3',source/'tests/function3/integration/runtime_blackbox.py','--allow-isolated-admin','--webm-fixture-dir',root/'media-fixtures'],stdout=log,stderr=subprocess.STDOUT)
        run(network+['python3',source/'tests/function3/integration/broker_fault.py'],stdout=log,stderr=subprocess.STDOUT)
    run(['python3',controller,'stop'])
    results=root/'benchmarks';previous=root/('benchmarks-'+a.history_tag)
    if results.exists():
        assert not previous.exists(),'preserve both previous benchmark runs'
        results.rename(previous)
    gate=subprocess.run(['python3',str(source/'scripts/function3/validation/benchmark_matrix.py')],cwd=source).returncode
    print('Final performance gate exit',gate,flush=True)
    run(['python3',controller,'stop']);run(['python3',controller,'stop','--variant','baseline'])
    # Existing real model; checksum is computed locally, never inferred from a name.
    model=Path('/home/pikachu/phase11-models/phi3-mini-4k-instruct-cpu-int4')
    checksum=subprocess.check_output(['python3',str(source/'scripts/phase11/model_checksum.py'),str(model)],text=True).strip()
    pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
    connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_runtime_test'
    seed=[pg,connection,'-w','-qAt','-v','test_suffix=f3_'+uuid.uuid4().hex[:8],'-v','model_artifact='+model.name,'-v','model_checksum='+checksum,'-f',str(source/'tests/phase11/integration/seed_onnx.sql')]
    ai=int(subprocess.check_output(seed,text=True).strip().splitlines()[-1])
    # 种子默认 active；本轮改为候选，以真实后台异步试装流程激活，然后才开始推理。
    run([pg,connection,'-w','-qAtc',f"UPDATE model_versions SET state='candidate' WHERE node_id=(SELECT model_node_id FROM model_bindings WHERE ai_user_id={ai});UPDATE model_nodes SET active_version_id=NULL WHERE id=(SELECT model_node_id FROM model_bindings WHERE ai_user_id={ai})"])
    run(['python3',controller,'start','--with-mq','--tts','--ai'])
    pid=(root/'current.pid').read_text().strip()
    with (root/'ai-final.log').open('w') as log:
        run(network+['python3',source/'tests/function3/integration/ai_mixed.py','--ai-user-id',str(ai),'--server-pid',pid,'--output',root/'ai-mixed.json'],stdout=log,stderr=subprocess.STDOUT)
    print('Real existing ONNX model + ordinary chat passed',flush=True)
finally:
    run(['python3',controller,'stop']);run(['python3',controller,'stop','--variant','baseline'])
    run(['python3',source/'scripts/function3/dependencies/isolated_rocketmq.py','stop'])

# 最终 HTTP 后台操作也纳入完整 Debug/Sanitizer 版本，复用同一构建缓存。
for flavour in ('debug','asan'):
    run(['python3',source/'scripts/function3/validation/build_checks.py',flavour,'--clean-generated-after'])
