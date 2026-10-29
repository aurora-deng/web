"""Real WS command + SSE token stream + shared group history through the existing gRPC Worker."""
from pathlib import Path
import argparse
import importlib.util
import json
import sys
import threading
import time
import uuid

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('group_ai_runtime',ROOT/'tests/function3/integration/runtime_blackbox.py')
r=importlib.util.module_from_spec(spec);sys.modules[spec.name]=r;spec.loader.exec_module(r)
m=r.module

def main():
    p=argparse.ArgumentParser();p.add_argument('--ai-user-id',type=int,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert r.sql('SELECT current_database()')=='function3_runtime_test'
    prefix='groupai_'+uuid.uuid4().hex[:8];owner=r.BrowserClient('127.0.0.1',18094);peer=r.BrowserClient('127.0.0.1',18094)
    owner.register_and_login(prefix+'_owner','Group-test-only!');peer.register_and_login(prefix+'_peer','Group-test-only!')
    friendship=r.expect(owner,'POST','/api/friend-requests',{'receiverId':str(peer.user_id)},201)
    r.expect(peer,'POST','/api/friend-requests/'+str(friendship['id']),{'accept':'true'})
    conversation=r.expect(owner,'POST','/api/conversations',{'kind':'group','title':'真实群AI验证','memberIds':str(peer.user_id)+','+str(a.ai_user_id)},201)['id']
    members=r.expect(owner,'GET',f'/api/conversations/{conversation}/members');ai=next(x for x in members if int(x['userId'])==a.ai_user_id)
    mention=ai.get('username') or ai.get('displayName');assert mention and ai['aiAccount']
    socket,ws=owner.open_websocket();sse_socket,sse=owner.open_sse();socket.settimeout(180);sse_socket.settimeout(30)
    stopped=threading.Event();frames=[];errors=[]
    def drain():
        try:
            while not stopped.is_set():frames.append(ws.read_json())
        except Exception as error:
            if not stopped.is_set():errors.append(type(error).__name__)
    thread=threading.Thread(target=drain,daemon=True);thread.start();report={'passed':False,'aiUserId':a.ai_user_id,'runs':[]}
    try:
        reply=None
        for trigger in ('mention','reply'):
            request=prefix+'_'+trigger;body={'v':1,'type':'ai.generate','id':request,'to':a.ai_user_id,'conversationId':int(conversation),'content':'@'+mention+' 请用一句中文问候群友。' if trigger=='mention' else '请再用一句话向群友问好。','aiTrigger':trigger}
            if reply is not None:body['replyTo']=str(reply)
            started=time.monotonic();socket.sendall(m.masked_frame(json.dumps(body,ensure_ascii=False).encode()));first=None;pieces=[];deadline=time.monotonic()+180;completed=None
            while time.monotonic()<deadline:
                event=sse.read();name=next((line[7:] for line in event.splitlines() if line.startswith('event: ')),'')
                if name=='ai.token':
                    text=m.sse_json(event).get('text','');pieces.append(text)
                    if text and first is None:first=time.monotonic()-started
                if name=='ai.completed':completed=m.sse_json(event);break
            assert completed and completed.get('success') is True and not completed.get('cancelled') and ''.join(pieces),(trigger,completed)
            history=r.expect(owner,'GET',f'/api/conversations/{conversation}/messages?after=0&limit=100')
            assistants=[x for x in history if int(x['senderId'])==a.ai_user_id];assert assistants
            reply=assistants[-1]['id'];report['runs'].append({'trigger':trigger,'firstTokenObservedSeconds':round(first,3),'totalSeconds':round(time.monotonic()-started,3),'historyAiMessages':len(assistants),'streamedBytes':len(''.join(pieces).encode())})
        assert not errors,errors
        assert any(frame.get('type')=='ai.accepted' for frame in frames)
        report['passed']=True
    finally:
        stopped.set();socket.close();sse_socket.close();thread.join(timeout=3)
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print('PASS actual group AI targeted mention and AI-message reply, SSE token completion and persistent shared history')

if __name__=='__main__':main()
