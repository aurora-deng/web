"""Real HTTP/PostgreSQL checks on the existing isolated database only."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('tree_social_runtime',ROOT/'tests/function3/integration/runtime_blackbox.py')
runtime=importlib.util.module_from_spec(spec);sys.modules[spec.name]=runtime;spec.loader.exec_module(runtime)
expect=runtime.expect

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=18094);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert runtime.sql('SELECT current_database()')=='function3_runtime_test'
    author,stranger,other=[runtime.BrowserClient('127.0.0.1',a.port) for _ in range(3)]
    prefix='tree_social_'+uuid.uuid4().hex[:10]
    for i,c in enumerate([author,stranger,other]):c.register_and_login(prefix+'_'+str(i),'Isolated-test-only!')
    category=expect(author,'GET','/api/tree/categories')[0]['id']
    post=expect(author,'POST','/api/activities',{'requestId':prefix,'body':'所有登录用户都可以看的树上故事','destination':'tree','categoryId':category,'visibility':'members','publicConfirmed':'true'},201)
    profilePost=expect(author,'POST','/api/activities',{'requestId':prefix+'-profile','body':'主页动态，不能通过 members 字段绕过好友规则','destination':'profile','visibility':'members'},201)
    private=expect(author,'POST','/api/activities',{'requestId':prefix+'-private','body':'即使好友也不可见','destination':'profile','visibility':'private'},201)
    for visitor in [stranger,other]:
        assert expect(visitor,'GET','/api/activities/'+post['id'])['id']==post['id']
        assert [r['id'] for r in expect(visitor,'GET',f'/api/activities?author={author.user_id}')]==[post['id']]
        for suffix in ['', '/share']:
            expect(visitor,'GET','/api/activities/'+profilePost['id']+suffix,code=404)
        expect(visitor,'POST','/api/activities/'+profilePost['id']+'/favorite',{'enabled':'true'},404)
    card=expect(stranger,'GET',f'/api/tree/posts/{post["id"]}/author');assert not card['allowTreeFriendRequests'] and not card['canRequestFriendship']
    expect(stranger,'POST',f'/api/tree/posts/{post["id"]}/friend-request',{},403)
    expect(author,'PATCH','/api/me/profile-display',{'showBiography':'true','showAvatar':'true','allowTreeFriendRequests':'true'})
    # Existing clients omitting the new setting must not silently clear consent.
    expect(author,'PATCH','/api/me/profile-display',{'showBiography':'true','showAvatar':'true'})
    assert expect(stranger,'GET',f'/api/tree/posts/{post["id"]}/author')['canRequestFriendship']
    expect(author,'POST',f'/api/tree/posts/{post["id"]}/friend-request',{},400)
    expect(stranger,'POST',f'/api/tree/posts/{profilePost["id"]}/friend-request',{},404)
    request=expect(stranger,'POST',f'/api/tree/posts/{post["id"]}/friend-request',{},201)
    expect(stranger,'POST',f'/api/tree/posts/{post["id"]}/friend-request',{},409)
    assert expect(stranger,'GET',f'/api/tree/posts/{post["id"]}/author')['pendingRequest']
    expect(author,'POST','/api/friend-requests/'+request['id'],{'accept':'true'})
    assert expect(stranger,'GET',f'/api/profiles/{author.user_id}')['audience']=='friends'
    assert {r['id'] for r in expect(stranger,'GET',f'/api/activities?author={author.user_id}')}=={post['id'],profilePost['id']}
    expect(stranger,'GET','/api/activities/'+private['id'],code=404)
    expect(author,'PATCH','/api/me/profile-display',{'showBiography':'true','showAvatar':'true','allowTreeFriendRequests':'false'})
    expect(other,'POST',f'/api/tree/posts/{post["id"]}/friend-request',{},403)
    expect(author,'POST',f'/api/blocks/{stranger.user_id}',{})
    expect(stranger,'GET','/api/activities/'+profilePost['id'],code=404)
    # API consent is enforced on the server, independently from the checkbox UI.
    body={'requestId':prefix,'aiUserId':'1','messageCount':'1','role0':'user','content0':'public probe','maxOutputTokens':'8'}
    expect(other,'POST','/api/ai/web/generations',body,403)
    expect(other,'GET','/api/ai/private/models',code=403)
    expect(other,'GET','/api/ai/web/models')
    guest=runtime.BrowserClient('127.0.0.1',a.port);expect(guest,'GET','/api/tree/posts',code=401)
    result={'passed':True,'database':'function3_runtime_test','checks':['tree visible across non-friend accounts','stranger author profiles restricted to tree stories','direct ID/share/favorite privacy','opt-in default off and legacy patch preserves consent','creator derived from post','self/private-source request denied','pending request deduplication','acceptance opens friend activities but not private posts','revocation and blocking','web consent required by server','private HTTPS requirement retained','anonymous access denied']}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n');print('PASS tree social privacy and web assistant consent real HTTP/PostgreSQL')

if __name__=='__main__':main()
