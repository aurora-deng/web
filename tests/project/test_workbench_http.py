"""Local HTTP checks, not browser automation. Uses only synthetic isolated data."""
import http.client
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT))
from backend.agent.program.SkillWorkbench.SkillWorkbench import make_server
from backend.agent.tool_runtime.registry import default_registry
from backend.agent.tool_runtime.runtime import ToolRuntime,MemoryAuditSink
from backend.agent.tool_runtime.skills import SkillRuntime
from backend.agent.tool_runtime.workspace import WorkspaceStore,bind_workspace

class HTTP(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base=ROOT/'build/workbench-http-tests';base.mkdir(parents=True,exist_ok=True)
        root=Path(tempfile.mkdtemp(prefix='contract-',dir=base))
        store=WorkspaceStore(root/'test.sqlite','owner');store.seed_demo()
        tools=ToolRuntime(default_registry(),MemoryAuditSink(10000));bind_workspace(tools,store)
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));cls.port=sock.getsockname()[1]
        cls.server=make_server(cls.port,SkillRuntime(tools,store));cls.thread=threading.Thread(target=cls.server.serve_forever,daemon=True);cls.thread.start()
        status,headers,raw=cls.request('GET','/');assert status==200
        cls.cookie=headers['set-cookie'].split(';',1)[0]
    @classmethod
    def tearDownClass(cls):cls.server.shutdown();cls.server.server_close();cls.thread.join(5)
    @classmethod
    def request(cls,method,path,body=None,headers=None):
        connection=http.client.HTTPConnection('127.0.0.1',cls.port,timeout=5)
        try:
            connection.request(method,path,body=body,headers=headers or {})
            response=connection.getresponse();return response.status,dict((k.lower(),v) for k,v in response.getheaders()),response.read()
        finally:connection.close()
    def auth(self,**extra):return {'Cookie':self.cookie,'Content-Type':'application/json','Origin':f'http://127.0.0.1:{self.port}',**extra}
    def test_assets_and_catalog_match_real_recipes(self):
        for path in ('/','/app.js','/style.css','/health/live'):
            status,headers,raw=self.request('GET',path);self.assertEqual(status,200);self.assertIn('nosniff',headers['x-content-type-options'])
        status,_,raw=self.request('GET','/api/catalog',headers=self.auth());self.assertEqual(status,200);self.assertEqual(len(json.loads(raw)['skills']),19)
    def test_auth_csrf_host_and_arbitrary_paths_fail_closed(self):
        self.assertEqual(self.request('GET','/api/catalog')[0],401)
        self.assertEqual(self.request('POST','/api/invoke',b'{}',self.auth(Origin='http://evil.invalid'))[0],403)
        self.assertEqual(self.request('GET','/',headers={'Host':'evil.invalid'})[0],403)
        self.assertEqual(self.request('GET','/../../auth.json',headers=self.auth())[0],404)
    def test_confirmed_write_and_replay_through_http(self):
        body={'skillId':'study.log_progress','parameters':{'date':'2026-11-25','subject':'systems','itemId':'q1','minutes':25,'idempotencyKey':'http-once'}}
        def call():
            status,_,raw=self.request('POST','/api/invoke',json.dumps(body),self.auth());self.assertEqual(status,200);return json.loads(raw)
        self.assertEqual(call()['error']['code'],'denied')
        body['confirm']=True;first=call();second=call();self.assertTrue(first['ok']);self.assertEqual(first['value'],second['value'])
    def test_invalid_duplicate_json_and_extra_fields_are_rejected(self):
        self.assertEqual(self.request('POST','/api/invoke',b'{"skillId":"a","skillId":"b"}',self.auth())[0],400)
        self.assertEqual(self.request('POST','/api/invoke',json.dumps({'skillId':'study.quiz','parameters':{'subject':'systems'},'shell':'x'}),self.auth())[0],400)
if __name__=='__main__':unittest.main(verbosity=2)
