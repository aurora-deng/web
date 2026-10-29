"""Real configured ONNX web assistant, actor isolation, consent and cancellation."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('web_assistant_runtime',ROOT/'tests/function3/integration/runtime_blackbox.py')
r=importlib.util.module_from_spec(spec);sys.modules[spec.name]=r;spec.loader.exec_module(r)
expect=r.expect

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--port',type=int,default=18094);a=p.parse_args()
    assert r.sql('SELECT current_database()')=='function3_runtime_test'
    user,other=[r.BrowserClient('127.0.0.1',a.port) for _ in range(2)];prefix='web_ai_'+uuid.uuid4().hex[:10]
    for i,c in enumerate([user,other]):c.register_and_login(prefix+'_'+str(i),'Web-assistant-test-only!')
    models=expect(user,'GET','/api/ai/web/models');assert models,'No configured ONNX model for real test'
    ai=models[0]['accountId'];before=r.sql(f'SELECT count(*) FROM messages WHERE sender_id={user.user_id}')
    def payload(key):return {'requestId':key,'aiUserId':ai,'messageCount':'1','role0':'user','content0':'Say OK.','maxOutputTokens':'8','nonConfidentialConsent':'true'}
    key=prefix+'-complete';body=payload(key);unconfirmed={**body,'nonConfidentialConsent':'false'}
    expect(user,'POST','/api/ai/web/generations',unconfirmed,403)
    start=time.monotonic();expect(user,'POST','/api/ai/web/generations',body,202)
    expect(user,'POST','/api/ai/web/generations',body,202)
    expect(user,'POST','/api/ai/web/generations',{**body,'content0':'Different content'},409)
    expect(other,'GET','/api/ai/web/generations/'+key,code=404)
    expect(user,'GET','/api/ai/private/generations/'+key,code=403)
    pieces=[];after=0;first=None;deadline=start+170
    while time.monotonic()<deadline:
        view=expect(user,'GET',f'/api/ai/web/generations/{key}?after={after}');pieces.extend(view['pieces']);after=view['next']
        if pieces and first is None:first=round(time.monotonic()-start,3)
        if view['state'] not in ('queued','running') and len(view['pieces'])<64:break
        time.sleep(.3)
    assert view['state']=='completed' and ''.join(pieces).strip(),(view['state'],view.get('error'))
    elapsed=round(time.monotonic()-start,3)
    cancel=prefix+'-cancel';expect(user,'POST','/api/ai/web/generations',payload(cancel),202)
    expect(user,'DELETE','/api/ai/web/generations/'+cancel,{})
    assert expect(user,'GET','/api/ai/web/generations/'+cancel)['state']=='cancelled'
    expect(other,'DELETE','/api/ai/web/generations/'+cancel,{},404)
    assert r.sql(f'SELECT count(*) FROM messages WHERE sender_id={user.user_id}')==before
    result={'passed':True,'database':'function3_runtime_test','modelAccount':ai,'firstTokenSeconds':first,'completeSeconds':elapsed,'answerBytes':len(''.join(pieces).encode()),'checks':['real configured model completion','server rejects missing consent','idempotent retry and payload conflict','different actor denied','private HTTP endpoint remains denied','cancellation and other actor denial','no server chat message persisted'],'cloudApi':'not tested with real key; browser fixture covers protocol success/failure'}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))

if __name__=='__main__':main()
