"""Phase45 real HTTP/WS + PostgreSQL contract. Existing isolated runtime DB only."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import uuid
from concurrent.futures import ThreadPoolExecutor

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('workspace_runtime',ROOT/'tests/function3/integration/runtime_blackbox.py')
r=importlib.util.module_from_spec(spec);sys.modules[spec.name]=r;spec.loader.exec_module(r)
expect=r.expect

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=18094);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert r.sql('SELECT current_database()')=='function3_runtime_test'
    clients=[r.BrowserClient('127.0.0.1',a.port) for _ in range(3)]
    prefix='ws_'+uuid.uuid4().hex[:10]
    for i,c in enumerate(clients):c.register_and_login(prefix+'_'+str(i),'Isolated-only-password!')
    owner,member,other=clients
    found=expect(owner,'GET',f'/api/users/search?q={prefix}&limit=1');assert len(found['items'])==1
    assert found['items'][0]['id']==str(member.user_id) and found['nextCursor']==str(member.user_id)
    assert set(found['items'][0])=={'id','username','displayName','avatarUrl','aiAccount','friendship','pendingRequest','canRequestFriendship'}
    assert expect(owner,'GET',f'/api/users/search?q={prefix}&after={found["nextCursor"]}')['items'][0]['id']==str(other.user_id)
    guest=r.BrowserClient('127.0.0.1',a.port);expect(guest,'GET','/api/users/search?q=x',code=401)
    expect(owner,'GET','/api/users/search?q=x&limit=999',code=400)
    group=expect(owner,'POST','/api/conversations',{'kind':'group','title':prefix,'memberIds':str(member.user_id)},201);cid=group['id']
    secret=expect(other,'POST','/api/conversations',{'kind':'group','title':prefix+'-hidden','memberIds':str(other.user_id)},201)
    sock,reader=owner.open_websocket()
    def command(client_socket,client_reader,payload,kind):
        client_socket.sendall(r.module.masked_frame(json.dumps(payload).encode()))
        for _ in range(1000):
            event=client_reader.read_json()
            if event.get('type')==kind and event.get('replyTo')==payload['id']:return event
        raise AssertionError('missing matching ACK')
    try:
        records=[]
        for i in range(4):records.append(command(sock,reader,{'v':1,'type':'chat.send','id':prefix+str(i),'conversationId':int(cid),'content':'needle 100% '+str(i)},'chat.accepted'))
        page=expect(member,'GET','/api/search/messages/page?q=needle&limit=2');assert len(page['items'])==2
        older=expect(member,'GET',f'/api/search/messages/page?q=needle&limit=2&before={page["nextCursor"]}')
        assert len(older['items'])==2 and not ({m['id'] for m in page['items']}&{m['id'] for m in older['items']})
        expect(other,'GET',f'/api/conversations/{cid}/messages/{records[0]["id"]}/context',code=403)
        expect(member,'GET',f'/api/search/messages/page?q=needle&conversationId={secret["id"]}',code=403)
        context=expect(member,'GET',f'/api/conversations/{cid}/messages/{records[1]["id"]}/context?radius=1')
        assert len(context['items'])==3 and str(context['target']['id'])==str(records[1]['id'])
        expect(member,'GET',f'/api/conversations/{cid}/messages/999999999/context',code=404)
        original_csrf=member.csrf;member.csrf='wrong';expect(member,'PATCH',f'/api/conversations/{cid}/preferences',{'notificationsMuted':'true'},403);member.csrf=original_csrf
        mute=expect(member,'PATCH',f'/api/conversations/{cid}/preferences',{'notificationsMuted':'true'})
        assert mute['notificationsMuted'] and mute['version']=='1'
        assert expect(member,'PATCH',f'/api/conversations/{cid}/preferences',{'notificationsMuted':'true'})['version']=='1'
        events=lambda:r.sql(f"SELECT count(*) FROM outbox_events WHERE event_type='conversation.preferences' AND aggregate_id={int(member.user_id)}")
        before_events=events()
        with ThreadPoolExecutor(4) as pool:
            replies=list(pool.map(lambda _:expect(member,'PATCH',f'/api/conversations/{cid}/preferences',{'notificationsMuted':'true'}),range(8)))
        assert all(value['version']=='1' for value in replies) and events()==before_events
        assert not expect(owner,'GET',f'/api/conversations/{cid}/preferences')['notificationsMuted']
        expect(other,'PATCH',f'/api/conversations/{cid}/preferences',{'notificationsMuted':'true'},403)
        extra=command(sock,reader,{'v':1,'type':'chat.send','id':prefix+'muted','conversationId':int(cid),'content':'still delivered while muted'},'chat.accepted')
        summary=next(c for c in expect(member,'GET','/api/conversations') if c['id']==cid)
        assert summary['notificationsMuted'] and summary['unreadCount']==5 and summary['latestSequence']=='5' and summary['preferencesVersion']=='1'
        assert expect(member,'GET',f'/api/conversations/{cid}/messages?after=4')[0]['id']==int(extra['id'])
        command(sock,reader,{'v':1,'type':'chat.recall','id':prefix+'recall','conversationId':int(cid),'replyTo':records[0]['id']},'chat.recall.accepted')
        context=expect(member,'GET',f'/api/conversations/{cid}/messages/{records[0]["id"]}/context?radius=1')
        assert context['target']['state']=='recalled' and not context['target']['body']
        assert len(expect(member,'GET','/api/search/messages/page?q=needle')['items'])==3
        expect(member,'POST',f'/api/conversations/{cid}/leave',{})
        assert expect(member,'GET','/api/search/messages/page?q=needle')['items']==[]
        expect(member,'GET',f'/api/conversations/{cid}/messages/{records[1]["id"]}/context',code=403)
        capabilities=expect(owner,'GET','/api/client-capabilities');assert capabilities['uploads']['chat']['maxBytes']==25*1024*1024
        assert capabilities['uploads']['treeVoice']['maxDurationMs']==60000
    finally:sock.close()
    result={'passed':True,'database':'function3_runtime_test','checks':['public user search/cursor/self exclusion','safe directory DTO','auth/CSRF/member isolation','global search stable pages','bounded context','same-value preference idempotence','per-member mute independent of delivery/unread','recalled content redaction','permission revocation on leave','server upload capabilities']}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n');print('PASS workspace real HTTP/WS/PostgreSQL')

if __name__=='__main__':main()
