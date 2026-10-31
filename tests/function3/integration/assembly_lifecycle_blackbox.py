"""Actual app startup failures with existing test PG; no formal services/data.

Exercises domain resources already constructed when TLS/listen/gRPC setup fails.
Every child is Popen-owned. Sanitizer diagnostics, aborts and hanging shutdown fail.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--server',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();binary=a.server.resolve();assert binary.is_relative_to(ROOT)
data=Path('/home/pikachu/phase11-data/function3/validation')
assert ROOT==Path('/home/pikachu/function3-src')
connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test'
key=data/'current.token.key';assert key.is_file()
env={k:v for k,v in os.environ.items() if not k.startswith(('WEB_','ASAN_','UBSAN_'))}
env.update(WEB_PHASE11_DATABASE=connection,WEB_PHASE11_TOKEN_KEY_FILE=str(key),WEB_FUNCTION3_ENABLE='1',
           WEB_PHASE11_DB_WORKERS='2',WEB_PHASE11_DB_POOL='2',WEB_HTTP_WORKERS='2',WEB_WS_WORKERS='2',
           WEB_SERVER_REACTORS='1',WEB_GRPC_ADDRESS='off',WEB_PHASE11_FILE_ROOT=str(data/'phase49/lifecycle-files'),
           WEB_FUNCTION3_MEDIA_ROOT=str(data/'phase49/lifecycle-media'))
if 'build-asan' in binary.parts:
    env.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
report={'passed':False,'binary':str(binary),'database':'function3_phase11_test','cases':[]}
a.output.parent.mkdir(parents=True,exist_ok=True)

def run(name,settings):
    started=time.monotonic()
    child=subprocess.Popen([str(binary)],cwd=ROOT,env={**env,**settings},stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    try:output=child.communicate(timeout=30)[0].decode(errors='replace')
    except subprocess.TimeoutExpired:
        # Exact owned child only; never select a listener by port/name.
        child.kill();child.communicate();raise AssertionError('domain assembly did not terminate: '+name)
    assert child.returncode==1,(name,child.returncode,output[-2000:])
    assert 'webserver failed to start:' in output,(name,output)
    if name.endswith('-after-assembly'):
        assert 'Phase 11 authentication enabled' in output,(name,'failure occurred before domain assembly',output)
    assert not any(token in output for token in ('ERROR: AddressSanitizer','ERROR: LeakSanitizer','runtime error:','terminate called','DEADLYSIGNAL')),(name,output)
    (a.output.parent/(name+'.log')).write_text(output,encoding='utf-8')
    report['cases'].append({'name':name,'exitCode':child.returncode,'seconds':round(time.monotonic()-started,3)})

try:
    # Constructor unwinding: store/executor exist before identity key fails.
    run('missing-private-key',{'WEB_PHASE11_TOKEN_KEY_FILE':str(data/'phase49/does-not-exist.key')})
    # Network start fails after community/model/outbox runtimes were constructed.
    run('invalid-tls-after-assembly',{'WEB_TLS_CERT':str(data/'phase49/does-not-exist.crt'),'WEB_TLS_KEY':str(data/'phase49/does-not-exist.pem')})
    with socket.socket() as blocker:
        blocker.bind(('127.0.0.1',0));blocker.listen(1)
        run('occupied-port-after-assembly',{'WEB_SERVER_PORT':str(blocker.getsockname()[1])})
        run('occupied-grpc-after-assembly',{'WEB_GRPC_ADDRESS':'127.0.0.1:'+str(blocker.getsockname()[1])})
    report['passed']=True
finally:a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PASS partial constructor / TLS / listen failure cleanup')
