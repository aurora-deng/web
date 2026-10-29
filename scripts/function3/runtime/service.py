"""Daily Function3 service: reuse phase11 on 5432, existing files/key and HTTP 18090.

Never creates a database, seeds accounts, downloads packages, stops PostgreSQL,
changes firewall rules or controls the old server. `prepare` backs up before DDL.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import shutil
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('action',choices=('prepare','start','stop','status'))
p.add_argument('--no-speech',action='store_true')
p.add_argument('--no-ai',action='store_true')
p.add_argument('--no-redis',action='store_true')
p.add_argument('--redis-port',type=int,default=6379,help='Existing Redis endpoint; does not start or reconfigure Redis')
p.add_argument('--mq-endpoint',default='',help='Optional already reachable Broker Proxy; empty means explicit local durable delivery')
a=p.parse_args()
source=Path(__file__).resolve().parents[3]
root=Path('/home/pikachu/phase11-data/function3/runtime')
root.mkdir(parents=True,exist_ok=True,mode=0o700)
os.chmod(root,0o700)
lock=(root/'service.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
pg=Path('/home/pikachu/phase11-deps/postgresql/18.6/bin')
connection='host=/home/pikachu/phase11-data/postgresql/run port=5432 user=pikachu dbname=phase11 connect_timeout=3'
binary=source/'build-release/webserver'
pidfile=root/'webserver.pid';ticksfile=root/'webserver.ticks'

def query(sql):
    return subprocess.check_output([str(pg/'psql'),connection,'-w','-X','-At','-v','ON_ERROR_STOP=1','-c',sql],text=True).strip()

def owned():
    if not pidfile.exists():return None
    pid=int(pidfile.read_text());process=Path('/proc')/str(pid)
    try:
        stat=(process/'stat').read_text().split()
        if stat[2]=='Z':return None
        command=(process/'cmdline').read_bytes().split(b'\0')[0]
        if not command:return None
        if command.decode()!=str(binary) or not ticksfile.exists() or stat[21]!=ticksfile.read_text().strip():
            raise RuntimeError('PID identity changed; refusing to control this process')
    except FileNotFoundError:return None
    return pid

def accounts_fingerprint():
    # Never print credentials or password hashes. Compare a digest around migrations.
    rows=query("SELECT id,username,password_hash FROM users ORDER BY id")
    return hashlib.sha256(rows.encode()).hexdigest()

if a.action=='status':
    print(json.dumps({'pid':owned(),'http':18090,'database':'phase11','postgresPort':5432,'log':str(root/'webserver.log')}))
elif a.action=='stop':
    if pid:=owned():
        os.kill(pid,signal.SIGTERM);deadline=time.monotonic()+45
        while owned() and time.monotonic()<deadline:time.sleep(.1)
        if owned():raise RuntimeError('Graceful shutdown timed out; inspect log, no force kill performed')
    print('Function3 stopped. Shared PostgreSQL and all data preserved.')
else:
    subprocess.run(['bash',str(source/'scripts/phase11/postgres-user-service.sh'),'start'],check=True)
    if query('SELECT current_database()')!='phase11':raise RuntimeError('Wrong database; refusing to continue')
    applied=set(query('SELECT version FROM phase11_schema_migrations').splitlines())
    migrations=sorted((source/'backend/application/storage/postgres/migrations').glob('*.sql'))
    missing=[m for m in migrations if str(int(m.name.split('_')[0])) not in applied]
    if a.action=='prepare':
        if owned():raise RuntimeError('Stop this Function3 service before migrations')
        before=accounts_fingerprint();users=query('SELECT count(*) FROM users');backup=None
        if missing:
            directory=root/'backups';directory.mkdir(exist_ok=True,mode=0o700)
            backup=directory/('phase11-before-'+time.strftime('%Y%m%dT%H%M%S')+'-'+secrets.token_hex(3)+'.dump')
            fd=os.open(backup,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
            with os.fdopen(fd,'wb') as output:subprocess.run([str(pg/'pg_dump'),connection,'-w','-Fc'],stdout=output,check=True)
            subprocess.run([str(pg/'pg_restore'),'--list',str(backup)],stdout=subprocess.DEVNULL,check=True)
            for migration in missing:
                subprocess.run([str(pg/'psql'),connection,'-w','-X','-v','ON_ERROR_STOP=1','-f',str(migration)],check=True,stdout=subprocess.DEVNULL)
        after=accounts_fingerprint()
        if before!=after:raise RuntimeError('Account identity/password digest changed; inspect backup before starting')
        record={'database':'phase11','users':int(users),'accountsUnchanged':True,'accountFingerprint':after,'applied':[m.name for m in missing],'backup':str(backup) if backup else None}
        (root/'migration-report.json').write_text(json.dumps(record,indent=2)+'\n')
        print(json.dumps(record))
    else:
        if missing:raise RuntimeError('Database needs migrations: run service.py prepare first; no automatic DDL or new database')
        if owned():raise RuntimeError('Function3 already running')
        if not binary.is_file():raise RuntimeError('Build the current Release binary first')
        key=Path('/home/pikachu/phase11-data/secrets/phase11-token-hash.key')
        if not key.is_file():raise RuntimeError('Original token key missing; refusing to replace it with a new key')
        cert=root/'localhost.crt';private=root/'localhost.key'
        if not cert.exists() or subprocess.run(['openssl','x509','-in',str(cert),'-noout','-checkend','3600'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','30','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1,IP:192.168.239.135','-keyout',str(private),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            private.chmod(0o600)
        env={k:v for k,v in os.environ.items() if not k.startswith(('WEB_PHASE11_','WEB_FUNCTION3_','WEB_GRPC_','WEB_TLS_'))}
        env.update({'WEB_SERVER_PORT':'18090','WEB_SERVER_REACTORS':'2','WEB_HTTP_WORKERS':'4','WEB_WS_WORKERS':'4','WEB_PHASE11_DATABASE':connection,'WEB_PHASE11_TOKEN_KEY_FILE':str(key),'WEB_PHASE11_FILE_ROOT':'/home/pikachu/phase11-data/files','WEB_PHASE11_DB_POOL':'4','WEB_PHASE11_DB_WORKERS':'4','WEB_FUNCTION3_ENABLE':'1','WEB_FUNCTION3_MEDIA_ROOT':'/home/pikachu/phase11-data/function3/media','WEB_TLS_PORT':'18443','WEB_TLS_CERT':str(cert),'WEB_TLS_KEY':str(private)})
        if not a.no_ai:env.update({'WEB_PHASE11_ONNX_MODEL_ROOT':'/home/pikachu/phase11-models','WEB_PHASE11_ONNX_WORKERS':'1','WEB_PHASE11_ONNX_MODELS':'2','WEB_PHASE11_ONNX_QUEUE':'4'})
        if not a.no_speech:
            worker=source/'build-release/backend/application/modules/function3-tts-worker'
            if not worker.is_file():raise RuntimeError('Speech worker missing; build it or explicitly use --no-speech')
            env['WEB_FUNCTION3_TTS_WORKER']=str(worker)
        if not 1<=a.redis_port<=65535:raise RuntimeError('invalid Redis port')
        if not a.no_redis:env.update({'WEB_FUNCTION3_REDIS_PORT':str(a.redis_port),'WEB_FUNCTION3_REDIS_PREFIX':'nebula:phase11:function3:'})
        if a.mq_endpoint:env['WEB_FUNCTION3_MQ_ENDPOINT']=a.mq_endpoint
        # 仅在显式启动且确认没有运行实例后准备日常资源；构建不再更新运行页面。
        working=root/'work';working.mkdir(exist_ok=True)
        shutil.copytree(source/'frontend/phase11',working/'frontend/phase11',dirs_exist_ok=True)
        shutil.copytree(source/'static',working/'static',dirs_exist_ok=True)
        with (root/'webserver.log').open('ab') as log:process=subprocess.Popen([str(binary)],cwd=working,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        pidfile.write_text(str(process.pid));ticksfile.write_text((Path('/proc')/str(process.pid)/'stat').read_text().split()[21])
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            if process.poll() is not None:raise RuntimeError('Startup failed; inspect '+str(root/'webserver.log'))
            if subprocess.run(['curl','--noproxy','*','-fsS','--max-time','1','http://127.0.0.1:18090/health/ready'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:break
            time.sleep(.2)
        else:raise RuntimeError('Readiness timed out; inspect log; no other process was stopped')
        print(json.dumps({'pid':process.pid,'http':18090,'database':'phase11','speech':not a.no_speech,'redis':not a.no_redis,'delivery':'rocketmq' if a.mq_endpoint else 'local','log':str(root/'webserver.log')}))
