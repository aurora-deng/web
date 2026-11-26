"""Single-owner, loopback-only development workbench. Not product authentication or a public API."""
import argparse
from dataclasses import asdict
from http import cookies
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import threading
import time
from urllib.parse import urlsplit

from ...tool_runtime.registry import default_registry,Effect
from ...tool_runtime.runtime import ToolRuntime,SqliteAuditSink,ToolError,encode
from ...tool_runtime.skills import SkillRuntime,FleetSimulationBridge
from ...tool_runtime.workspace import WorkspaceStore,NativeFleetPort,bind_workspace,connect

ROOT=Path(__file__).resolve().parents[4]
ASSETS=ROOT/'frontend/agent-workbench'

class BoundedServer(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self,*args,**kwargs):
        self.slots=threading.BoundedSemaphore(8);super().__init__(*args,**kwargs)
    def process_request(self,request,address):
        if not self.slots.acquire(blocking=False):
            try:request.sendall(b'HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
            finally:self.shutdown_request(request)
            return
        try:super().process_request(request,address)
        except Exception:self.slots.release();raise
    def process_request_thread(self,*args):
        try:super().process_request_thread(*args)
        finally:self.slots.release()


def make_server(port,skills):
    sessions={};lock=threading.Lock()
    origins={f'http://127.0.0.1:{port}',f'http://localhost:{port}'}
    hosts={f'127.0.0.1:{port}',f'localhost:{port}'}
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*_):pass
        def setup(self):
            super().setup();self.connection.settimeout(5)
        def reply(self,status,body,kind='application/json; charset=utf-8',cookie=None):
            raw=body if isinstance(body,bytes) else encode(body)
            self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(raw)))
            self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff')
            self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
            if cookie:self.send_header('Set-Cookie',f'agent_preview={cookie}; HttpOnly; SameSite=Strict; Path=/; Max-Age=3600')
            self.end_headers();self.wfile.write(raw)
        def valid_host(self):return self.headers.get('Host') in hosts
        def authenticated(self):
            try:
                jar=cookies.SimpleCookie();jar.load(self.headers.get('Cookie',''));token=jar['agent_preview'].value
            except (KeyError,cookies.CookieError):return False
            with lock:return sessions.get(token,0)>time.monotonic()
        def do_GET(self):
            if not self.valid_host():return self.reply(403,{'error':'denied'})
            path=urlsplit(self.path).path
            if path=='/health/live':return self.reply(200,{'status':'ok','mode':'isolated-loopback-preview','skills':len(skills.recipes)})
            if path in {'/','/app.js','/style.css'}:
                target={'/':('index.html','text/html; charset=utf-8'),'/app.js':('app.js','text/javascript; charset=utf-8'),'/style.css':('style.css','text/css; charset=utf-8')}[path]
                cookie=None
                if path=='/' and not self.authenticated():
                    with lock:
                        now=time.monotonic()
                        for token,expires in list(sessions.items()):
                            if expires<=now:del sessions[token]
                        if len(sessions)>=16:return self.reply(503,{'error':'session_limit'})
                        cookie=secrets.token_urlsafe(32);sessions[cookie]=now+3600
                return self.reply(200,(ASSETS/target[0]).read_bytes(),target[1],cookie)
            if not self.authenticated():return self.reply(401,{'error':'denied'})
            if path=='/api/catalog':
                return self.reply(200,{'mode':'isolated-preview','planner':'rules','skills':[
                    {'name':s.name,'version':s.version,'effect':s.effect.value,'parameters':s.parameters,
                     'preconditions':[c.value for c in s.preconditions],'timeoutMs':s.timeout_ms,'description':s.description}
                    for s in skills.registry.skills.values()]})
            if path=='/api/proposals':
                with connect(skills.store.database,True) as db:
                    rows=[dict(r) for r in db.execute('SELECT id,skill,state FROM proposals WHERE actor=? ORDER BY created DESC LIMIT 50',(skills.store.owner,))]
                return self.reply(200,{'proposals':rows})
            return self.reply(404,{'error':'not_found'})
        def do_POST(self):
            if not self.valid_host() or self.headers.get('Origin') not in origins or not self.authenticated():return self.reply(403,{'error':'denied'})
            if self.headers.get('Transfer-Encoding') or self.headers.get_content_type()!='application/json':return self.reply(400,{'error':'invalid'})
            try:
                length=int(self.headers.get('Content-Length','0'))
                if not 0<length<=16384:raise ValueError()
                def unique(pairs):
                    obj={}
                    for k,v in pairs:
                        if k in obj:raise ValueError()
                        obj[k]=v
                    return obj
                body=json.loads(self.rfile.read(length),object_pairs_hook=unique)
                if not isinstance(body,dict):raise ValueError()
                path=urlsplit(self.path).path
                if path=='/api/invoke':
                    if not {'skillId','parameters'}<=body.keys() or set(body)-{'skillId','parameters','version','confirm'}:raise ValueError()
                    version=body.get('version',1);proof=None
                    if body.get('confirm') is True:
                        proof=skills.confirm(skills.store.owner,body['skillId'],body['parameters'],version)
                    elif 'confirm' in body and body['confirm'] is not False:raise ValueError()
                    result=skills.invoke(body['skillId'],body['parameters'],version=version,actor_id=skills.store.owner,confirmation=proof)
                    return self.reply(200,result)
                if path=='/api/plan':
                    if set(body)!={'intent','parameters'}:raise ValueError()
                    return self.reply(200,skills.planner.plan(body['intent'],body['parameters']))
                if path=='/api/decide':
                    if set(body)!={'proposalId','approve'}:raise ValueError()
                    return self.reply(200,skills.decide(body['proposalId'],actor_id=skills.store.owner,approve=body['approve']))
                return self.reply(404,{'error':'not_found'})
            except ToolError as error:return self.reply(400,{'error':error.code})
            except (ValueError,TypeError,KeyError,RecursionError):return self.reply(400,{'error':'invalid'})
            except Exception:return self.reply(503,{'error':'unavailable'})
    return BoundedServer(('127.0.0.1',port),Handler)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=18125);parser.add_argument('--state',type=Path,required=True)
    parser.add_argument('--fleet-ctl',type=Path,required=True);parser.add_argument('--workflow-checkpoint',type=Path,required=True)
    parser.add_argument('--fleet-agent',type=Path,required=True);parser.add_argument('--fleet-address',required=True)
    parser.add_argument('--demo',action='store_true');args=parser.parse_args()
    if not 1024<=args.port<=65535:parser.error('invalid port')
    state=args.state.absolute()
    if any(p.is_symlink() for p in (state,*state.parents)) or state.is_relative_to(ROOT):parser.error('state must be a real directory outside source')
    os.umask(0o077);state.mkdir(mode=0o700,parents=False,exist_ok=True)
    readenv={k:v for k,v in os.environ.items() if not k.startswith('NEBULA_FLEET_')}
    readenv['NEBULA_FLEET_OPERATOR_TOKEN']=os.environ['NEBULA_FLEET_AGENT_READ_TOKEN']
    snapshot=NativeFleetPort(args.fleet_ctl,args.fleet_address,readenv,args.workflow_checkpoint)
    store=WorkspaceStore(state/'workspace.sqlite','preview-owner',snapshot)
    if args.demo:store.seed_demo()
    tools=ToolRuntime(default_registry(),SqliteAuditSink(state/'audit.sqlite'));bind_workspace(tools,store)
    # The supervisor supplies newly generated simulation credentials; never print them.
    allowed={'NEBULA_FLEET_AGENT_READ_TOKEN','NEBULA_FLEET_AGENT_DRAFT_TOKEN','NEBULA_FLEET_AGENT_REVIEW_TOKEN'}
    env={k:v for k,v in os.environ.items() if not k.startswith('NEBULA_FLEET_') or k in allowed}
    bridge=FleetSimulationBridge(args.fleet_agent,args.fleet_address,env)
    server=make_server(args.port,SkillRuntime(tools,store,bridge))
    print(f'READY 127.0.0.1:{args.port} isolated Agent workbench',flush=True)
    try:server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:pass
    finally:server.server_close()
if __name__=='__main__':main()
