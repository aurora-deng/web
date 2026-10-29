"""Control only this task's loopback/TLS ONNX Worker; never install or touch daily services."""
import argparse
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import time

SOURCE=Path('/home/pikachu/function3-src')
ROOT=Path('/home/pikachu/phase11-data/function3/validation/onnx-worker')
BINARY=SOURCE/'build-release/webserver-onnx-worker'
CERT=Path('/home/pikachu/phase11-data/function3/runtime/localhost.crt')

def token():
    ROOT.mkdir(parents=True,exist_ok=True,mode=0o700)
    path=ROOT/'rpc-token'
    if not path.exists():
        with path.open('x') as output:output.write(secrets.token_hex(32)+'\n')
        path.chmod(0o600)
    if path.stat().st_mode&0o077:raise RuntimeError('Worker token must be private')
    return path.read_text().strip()

def owned():
    if not (ROOT/'pid').exists():return None
    pid=int((ROOT/'pid').read_text());proc=Path('/proc')/str(pid)
    try:
        stat=(proc/'stat').read_text().split()
        if stat[2]=='Z':return None
        command=(proc/'cmdline').read_bytes().decode().replace('\0',' ')
    except (FileNotFoundError,ProcessLookupError):return None
    if str(BINARY) not in command or stat[21]!=(ROOT/'ticks').read_text():raise RuntimeError('Worker PID identity mismatch')
    return pid

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=('start','stop','status'));args=parser.parse_args()
    ROOT.mkdir(parents=True,exist_ok=True,mode=0o700)
    if args.action=='status':print('Isolated ONNX Worker PID',owned());return
    if args.action=='stop':
        if pid:=owned():
            os.kill(pid,signal.SIGTERM);deadline=time.monotonic()+30
            while owned() and time.monotonic()<deadline:time.sleep(.1)
            if owned():raise RuntimeError('Worker drain timed out; no forced kill performed')
        return
    if owned():print('Isolated ONNX Worker already running');return
    if not BINARY.is_file() or not CERT.is_file() or not CERT.with_suffix('.key').is_file():raise RuntimeError('Existing Worker build/TLS files required; no downloads or certificate generation')
    env=os.environ.copy()
    for key in list(env):
        if key.startswith('WEB_PHASE11_'):env.pop(key)
    env.update({'WEB_PHASE11_ONNX_MODEL_ROOT':'/home/pikachu/phase11-models','WEB_PHASE11_ONNX_WORKERS':'1',
                'WEB_PHASE11_ONNX_MODELS':'1','WEB_PHASE11_ONNX_QUEUE':'2','WEB_PHASE11_ONNX_WORKER_ADDRESS':'127.0.0.1:50062',
                'WEB_PHASE11_ONNX_RPC_TOKEN':token(),'WEB_PHASE11_ONNX_WORKER_TLS_CERT':str(CERT),
                'WEB_PHASE11_ONNX_WORKER_TLS_KEY':str(CERT.with_suffix('.key'))})
    log=(ROOT/'worker.log').open('ab')
    process=subprocess.Popen([str(BINARY)],cwd=SOURCE,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    (ROOT/'pid').write_text(str(process.pid));(ROOT/'ticks').write_text((Path('/proc')/str(process.pid)/'stat').read_text().split()[21])
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if process.poll() is not None:raise RuntimeError('Worker exited; inspect private worker.log')
        try:
            with socket.create_connection(('127.0.0.1',50062),timeout=.2):break
        except OSError:time.sleep(.1)
    else:raise RuntimeError('Worker listener timed out')
    print('Isolated ONNX Worker listening on loopback 50062 with TLS and token; model loads on first use')

if __name__=='__main__':main()
