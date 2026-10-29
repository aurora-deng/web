"""Run only task-owned isolated servers/databases. Never controls the formal server."""
import argparse
import os
from pathlib import Path
import secrets
import signal
import subprocess
import shutil
import time
from process_identity import owned_process

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('action',choices=('start','stop','status'))
p.add_argument('--variant',choices=('current','baseline'),default='current')
p.add_argument('--with-mq',action='store_true')
p.add_argument('--tts',action='store_true')
p.add_argument('--ai',action='store_true',help='Reuse existing local ONNX model root without downloads')
p.add_argument('--ai-worker',action='store_true',help='Recommended independent ONNX gRPC Worker on isolated loopback TLS 50062; requires a bound onnx-grpc model version')
p.add_argument('--http-port',type=int,default=18094)
p.add_argument('--tls-port',type=int,default=18444,help='Isolated TLS listener; daily service keeps 18443')
p.add_argument('--runtime-certificate',action='store_true',help='Reuse the existing LAN-IP learning certificate; never imports client trust')
a=p.parse_args()
if a.ai and a.ai_worker:p.error('Choose --ai (teaching in-process) or --ai-worker (isolated inference), not both')
source=Path('/home/pikachu')/('function3-src' if a.variant=='current' else 'function3-baseline-src')
binary=source/'build-release/webserver'
root=Path('/home/pikachu/phase11-data/function3/validation');root.mkdir(parents=True,exist_ok=True,mode=0o700)
pidfile=root/(a.variant+'.pid');ticksfile=root/(a.variant+'.ticks')
def owned():
    if not pidfile.exists():return None
    return owned_process(int(pidfile.read_text()),ticksfile.read_text(),binary)
if a.action=='stop':
    if pid:=owned():
        os.kill(pid,signal.SIGTERM)
        deadline=time.monotonic()+45
        while owned() and time.monotonic()<deadline:time.sleep(.1)
        if owned():raise RuntimeError('isolated server did not stop within 45 seconds; inspect its log')
elif a.action=='status':print(a.variant,'PID',owned())
else:
    if owned():p.error('this isolated variant is already running')
    if not binary.exists():p.error('build the isolated binary first')
    # 契约/容量库允许重置自增 ID，真实 HTTP 库与文件目录必须保持独立，重启时稳定复用。
    db='function3_runtime_test' if a.variant=='current' else 'function3_baseline_test'
    pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
    socket='/home/pikachu/phase11-data/function3/pg/run'
    command=[pg,'-w','-h',socket,'-p','55434','-U','function3_test_admin','-d','postgres','-Atc']
    if not subprocess.check_output(command+[f"SELECT 1 FROM pg_database WHERE datname='{db}'"],text=True).strip():
        p.error('The designated existing test database is missing; no database created')
    applied=subprocess.check_output(command[:-3]+['-d',db,'-Atc',"SELECT to_regclass('public.phase11_schema_migrations')"],text=True).strip()
    if not applied:
        # 只初始化新创建、名称固定且以 _test 结尾的隔离库；正式库没有入口。
        for migration in sorted((source/'backend/application/storage/postgres/migrations').glob('*.sql')):
            subprocess.run([pg,'-w','-h',socket,'-p','55434','-U','function3_test_admin','-d',db,'-v','ON_ERROR_STOP=1','-f',str(migration)],check=True,stdout=subprocess.DEVNULL)
    key=root/(a.variant+'.token.key')
    if not key.exists():key.write_text(secrets.token_hex(32)+'\n');key.chmod(0o600)
    certificate=root/'localhost.crt';private=root/'localhost.key'
    if a.runtime_certificate:
        certificate=Path('/home/pikachu/phase11-data/function3/runtime/localhost.crt')
        private=certificate.with_suffix('.key')
        if not certificate.is_file() or not private.is_file():p.error('Existing runtime certificate is missing; no replacement generated')
    if not certificate.exists():
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','7','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1','-keyout',str(private),'-out',str(certificate)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        private.chmod(0o600)
    env=os.environ.copy()
    for name in list(env):
        if name.startswith(('WEB_PHASE11_','WEB_FUNCTION3_','WEB_GRPC_','WEB_TLS_')):env.pop(name)
    env.update({'WEB_SERVER_PORT':str(a.http_port),'WEB_SERVER_REACTORS':'2','WEB_HTTP_WORKERS':'4','WEB_WS_WORKERS':'4','WEB_GRPC_ADDRESS':'127.0.0.1:50064',
        'WEB_PHASE11_DATABASE':f'host={socket} port=55434 user=function3_test_admin dbname={db}',
        'WEB_PHASE11_TOKEN_KEY_FILE':str(key),'WEB_PHASE11_FILE_ROOT':str(root/(a.variant+'-runtime-files')),
        'WEB_FUNCTION3_MEDIA_ROOT':str(root/(a.variant+'-tree-media')),
        'WEB_PHASE11_DB_WORKERS':'4','WEB_PHASE11_DB_POOL':'4','WEB_FUNCTION3_ENABLE':'1' if a.variant=='current' else '0',
        'WEB_TLS_CERT':str(certificate),'WEB_TLS_KEY':str(private),'WEB_TLS_PORT':str(a.tls_port)})
    if a.ai:env.update({'WEB_PHASE11_ONNX_MODEL_ROOT':'/home/pikachu/phase11-models','WEB_PHASE11_ONNX_WORKERS':'1','WEB_PHASE11_ONNX_MODELS':'1','WEB_PHASE11_ONNX_QUEUE':'2'})
    if a.ai_worker:
        if a.variant!='current':p.error('Independent Worker validation is only for the current isolated server')
        subprocess.run(['python3',str(source/'scripts/function3/validation/onnx_worker.py'),'start'],check=True)
        token_path=root/'onnx-worker/rpc-token'
        env.update({'WEB_PHASE11_ONNX_GRPC_TARGET':'127.0.0.1:50062','WEB_PHASE11_ONNX_RPC_TOKEN':token_path.read_text().strip(),
                    'WEB_PHASE11_ONNX_GRPC_TLS_CA':'/home/pikachu/phase11-data/function3/runtime/localhost.crt',
                    'WEB_PHASE11_ONNX_GRPC_TLS_NAME':'192.168.239.135','WEB_PHASE11_ONNX_GRPC_WORKERS':'1',
                    'WEB_PHASE11_ONNX_GRPC_QUEUE':'2','WEB_PHASE11_ONNX_GRPC_STREAM_BYTES':'65536'})
    if a.with_mq:env['WEB_FUNCTION3_MQ_ENDPOINT']='127.0.0.1:20881'
    if a.tts:env['WEB_FUNCTION3_TTS_WORKER']=str(source/'build-release/backend/application/modules/function3-tts-worker')
    args=[str(binary)]
    namespace=[]
    if a.with_mq:
        ns=Path('/home/pikachu/phase11-data/function3/rocketmq/namespace.pid').read_text().strip()
        sameUser=os.readlink('/proc/self/ns/user')==os.readlink('/proc/'+ns+'/ns/user')
        sameNetwork=os.readlink('/proc/self/ns/net')==os.readlink('/proc/'+ns+'/ns/net')
        if not(sameUser and sameNetwork):
            namespace=['nsenter','--preserve-credentials','-t',ns]+([] if sameUser else ['-U'])+([] if sameNetwork else ['-n'])
        args=namespace+args
    log=(root/(a.variant+'.log')).open('ab')
    # 构建目录可能仍是旧日常实例的cwd。测试使用自己的页面快照和工作目录，
    # 防止一次构建/页面刷新顺带改掉18090实例实际读取的文件。
    working=root/(a.variant+'-work');working.mkdir(exist_ok=True)
    shutil.copytree(source/'frontend/phase11',working/'frontend/phase11',dirs_exist_ok=True)
    shutil.copytree(source/'static',working/'static',dirs_exist_ok=True)
    process=subprocess.Popen(args,cwd=working,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    pidfile.write_text(str(process.pid));ticksfile.write_text((Path('/proc')/str(process.pid)/'stat').read_text().split()[21])
    probe=['curl','-fsS','--max-time','1',f'http://127.0.0.1:{a.http_port}/health/live']
    if a.with_mq:probe=namespace+probe
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        if process.poll() is not None:raise RuntimeError('isolated server exited; inspect '+str(root/(a.variant+'.log')))
        if subprocess.run(probe,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:break
        time.sleep(.1)
    else:raise RuntimeError('isolated server readiness timed out')
    print('Started isolated',a.variant,'PID',process.pid,f'HTTP {a.http_port} / HTTPS {a.tls_port}')
