"""Real existing ONNX artifact over isolated loopback gRPC/TLS; no downloads or daily-service changes."""
from pathlib import Path
import argparse
import importlib.util
import json
import os
import signal
import ssl
import subprocess
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('worker_archive_helpers',ROOT/'tests/function3/integration/private_archive_blackbox.py')
h=importlib.util.module_from_spec(spec);sys.modules[spec.name]=h;spec.loader.exec_module(h)
controller=ROOT/'scripts/function3/validation/run_server.py';worker=ROOT/'scripts/function3/validation/onnx_worker.py'

def run(*args):subprocess.run(['python3',*map(str,args)],check=True)
def rss(pid):
    for line in (Path('/proc')/str(pid)/'status').read_text().splitlines():
        if line.startswith('VmRSS:'):return int(line.split()[1])/1024

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert h.runtime.sql('SELECT current_database()')=='function3_runtime_test'
    run(controller,'stop');run(worker,'stop');run(controller,'start','--runtime-certificate','--ai-worker')
    ctx=ssl.create_default_context(cafile='/home/pikachu/phase11-data/function3/runtime/localhost.crt');ctx.set_alpn_protocols(['http/1.1'])
    owner=h.Browser(ctx);ai=h.Browser(ctx);peer=h.Browser(ctx);prefix='worker_'+uuid.uuid4().hex[:8]
    owner.login(prefix+'_owner');ai.login(prefix+'_ai');peer.login(prefix+'_peer')
    friendship=owner.call('POST','/api/friend-requests',{'receiverId':str(peer.user)},201)
    peer.call('POST','/api/friend-requests/'+str(friendship['id']),{'accept':'true'})
    ai_id=int(ai.user);h.runtime.sql(f'UPDATE users SET ai_account=true WHERE id={ai_id}')
    # Reuse the exact previously checked artifact/checksum, but create a separate grpc-bound test fixture.
    node=int(h.runtime.sql(f"INSERT INTO model_nodes(name,kind,created_at) VALUES('{prefix}','specialist',now()) RETURNING id"))
    version=int(h.runtime.sql(f"INSERT INTO model_versions(node_id,runtime,model_artifact,adapter_artifact,checksum,state,created_at) SELECT {node},'onnx-grpc',model_artifact,adapter_artifact,checksum,'active',now() FROM model_versions WHERE id=1 RETURNING id"))
    h.runtime.sql(f'UPDATE model_nodes SET active_version_id={version} WHERE id={node}')
    h.runtime.sql(f'INSERT INTO model_bindings(ai_user_id,model_node_id) VALUES({ai_id},{node})')
    report={'runtime':'onnx-grpc','modelVersion':version,'modelNode':node,'aiUserId':ai_id,'artifactReused':True,'runs':[],'modelQualityEvaluated':False,'passed':False}
    def persist():
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    def start(tokens,prompt):
        return owner.call('POST','/api/ai/private/generations',{'requestId':uuid.uuid4().hex,'aiUserId':str(ai_id),'messageCount':'1','role0':'user','content0':prompt,'maxOutputTokens':str(tokens)},202)
    def complete(generation,started):
        first=None;deadline=time.monotonic()+175;cursor=0;pieces=[]
        while time.monotonic()<deadline:
            result=owner.call('GET',f'/api/ai/private/generations/{generation["id"]}?after={cursor}')
            if result['pieces'] and first is None:first=time.monotonic()-started
            pieces+=result['pieces'];cursor=result['next']
            if result['state'] not in ('queued','running'):break
            time.sleep(.05)
        assert result['state']=='completed' and pieces and result['serverPersistsContent'] is False,result
        return {'firstPollTokenSeconds':round(first,3),'serverFirstTokenMs':round(result['firstTokenMicroseconds']/1000,3),'totalSeconds':round(time.monotonic()-started,3),'state':result['state'],'outputBytes':sum(len(x.encode()) for x in pieces)}
    for state in ('cold','hot'):
        started=time.monotonic();generation=start(32,'用一句简短的中文问候我。');result=complete(generation,started);result['cache']=state
        pid=int(Path('/home/pikachu/phase11-data/function3/validation/onnx-worker/pid').read_text());result['workerRssMiB']=round(rss(pid),2);report['runs'].append(result);persist()
    # Abruptly kill only the identity-checked test Worker while a real generation is active.
    generation=start(2048,'请详细介绍网络服务器的设计，持续写作，不要立即停止。');deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        state=owner.call('GET',f'/api/ai/private/generations/{generation["id"]}')
        if state['pieces']:break
        if state['state']=='failed':raise AssertionError(state)
        time.sleep(.1)
    worker_spec=importlib.util.spec_from_file_location('owned_test_worker',worker);owned=importlib.util.module_from_spec(worker_spec);worker_spec.loader.exec_module(owned)
    pid=owned.owned();assert pid is not None;os.kill(pid,signal.SIGKILL)
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        state=owner.call('GET',f'/api/ai/private/generations/{generation["id"]}')
        if state['state']=='failed':break
        time.sleep(.1)
    assert state['state']=='failed',state
    assert owner.call('GET','/health/live')['status']=='alive'
    conversation=owner.call('POST','/api/conversations',{'kind':'direct','peerId':str(peer.user)},201)['id']
    message=owner.call('POST',f'/api/conversations/{conversation}/messages',{'clientMessageId':prefix+'_ordinary','text':'推理进程故障后，普通聊天继续持久化。'},201)
    assert message['id'] and h.runtime.sql(f'SELECT count(*) FROM messages WHERE sender_id={ai_id}')=='0'
    report['crashIsolation']={'signal':'SIGKILL to identity-checked isolated Worker','privateTask':'failed','serverLive':True,'ordinaryMessagePersisted':True,'privateAiMessagesInDatabase':0}
    persist()
    run(worker,'start');started=time.monotonic();result=complete(start(8,'你好'),started);result['cache']='after-worker-restart';report['runs'].append(result)
    report['passed']=True;persist()
    print('PASS real independent ONNX gRPC/TLS, cold/hot first-token measurement, abrupt Worker failure isolation and recovery')

if __name__=='__main__':main()
