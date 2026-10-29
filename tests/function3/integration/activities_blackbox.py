"""Real HTTP activity/privacy/legacy media checks; only the existing isolated DB.

No formal account/database/service changes. Reuses the project's original HTTP
client, file fixtures and already-installed PostgreSQL tools.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import time
import uuid

root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('f3_activity_runtime',root/'tests/function3/integration/runtime_blackbox.py')
module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
expect=module.expect


def main():
    p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=18094);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert module.sql('SELECT current_database()')=='function3_runtime_test'
    clients=[module.BrowserClient('127.0.0.1',a.port) for _ in range(4)]
    author,friend,stranger,admin=clients;prefix='activity_'+uuid.uuid4().hex[:10]
    for i,client in enumerate(clients):client.register_and_login(prefix+'_'+str(i),'Activity-test-only!')
    module.sql(f"INSERT INTO site_roles VALUES({admin.user_id},'administrator')")
    request=expect(author,'POST','/api/friend-requests',{'receiverId':str(friend.user_id)},201)
    expect(friend,'POST','/api/friend-requests/'+str(request['id']),{'accept':'true'})
    before=expect(author,'GET','/api/tree/posts')['total']
    payload={'requestId':prefix+'-friends','body':'只给好友看的生活','destination':'profile','visibility':'friends'}
    friends=expect(author,'POST','/api/activities',payload,201)
    assert expect(author,'POST','/api/activities',payload,201)['id']==friends['id']
    expect(friend,'GET','/api/activities/'+friends['id']);expect(stranger,'GET','/api/activities/'+friends['id'],code=404)
    expect(stranger,'POST','/api/activities/'+friends['id']+'/favorite',{'enabled':'true'},404)
    assert expect(author,'GET','/api/tree/posts')['total']==before
    private=expect(author,'POST','/api/activities',{**payload,'requestId':prefix+'-private','visibility':'private'},201)
    expect(friend,'GET','/api/activities/'+private['id'],code=404);expect(admin,'GET','/api/activities/'+private['id'],code=404)
    assert len(expect(friend,'GET',f'/api/activities?author={author.user_id}'))==1
    assert expect(stranger,'GET',f'/api/activities?author={author.user_id}')==[]
    image=module.upload(author,'activity.png','image/png',module.png())
    category=expect(author,'GET','/api/tree/categories')[0]['id']
    publicPayload={**payload,'requestId':prefix+'-public','destination':'both','visibility':'members','categoryId':category,'fileIds':image}
    expect(author,'POST','/api/activities',publicPayload,400)
    public=expect(author,'POST','/api/activities',{**publicPayload,'publicConfirmed':'true'},201)
    assert int(expect(author,'GET','/api/tree/posts')['total'])==int(before)+1
    batch=expect(stranger,'GET','/api/tree/posts?ids='+','.join([friends['id'],private['id'],public['id']]))
    assert [row['id'] for row in batch['posts']]==[public['id']]
    expect(stranger,'GET','/api/tree/posts?ids='+','.join([public['id']]*11),code=400)
    expect(friend,'POST',f'/api/tree/posts/{public["id"]}/like',{'liked':'true'})
    assert expect(stranger,'GET','/api/activities/'+public['id'])['likes']=='1'
    expect(friend,'POST','/api/activities/'+public['id']+'/favorite',{'enabled':'true'})
    expect(friend,'POST','/api/activities/'+public['id']+'/favorite',{'enabled':'true'})
    assert len(expect(friend,'GET','/api/activities/favorites')['items'])==1
    expect(friend,'GET','/api/activities/'+public['id']+'/share')
    assert module.raw(friend,'GET',public['media'][0]['url'])[0]==200
    expect(friend,'DELETE','/api/tree/posts/'+public['id'],{},403)
    expect(admin,'DELETE','/api/tree/posts/'+public['id'],{},400)
    expect(admin,'DELETE','/api/tree/posts/'+public['id'],{'reason':'隔离测试：权限与审计'})
    for route in ['/api/activities/'+public['id'],'/api/tree/posts/'+public['id'],'/api/activities/'+public['id']+'/share']:
        expect(friend,'GET',route,code=404)
    assert expect(friend,'GET','/api/tree/posts?ids='+public['id'])['posts']==[]
    assert module.raw(friend,'GET',public['media'][0]['url'])[0]==404
    assert expect(friend,'GET','/api/activities/favorites')['items']==[]
    expect(author,'POST',f'/api/blocks/{friend.user_id}',{})
    expect(friend,'GET','/api/activities/'+friends['id'],code=404)
    expect(author,'PATCH','/api/me/profile-display',{'showBiography':'false','showAvatar':'false'})
    profile=expect(friend,'GET',f'/api/profiles/{author.user_id}')
    assert profile['self'] is False and profile['biography']=='' and profile['avatarUrl']==''
    assert 'passwordHash' not in profile and 'tokenHash' not in profile
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        if int(module.sql(f"SELECT count(*) FROM site_audit WHERE actor_id={admin.user_id} AND action='delete'")):break
        time.sleep(.1)
    else:raise AssertionError('administrator deletion audit missing')
    report={'passed':True,'database':'function3_runtime_test','port':a.port,
        'checks':['friends/private isolation','shared content ID and interactions','explicit public confirmation',
            'tree count isolation','favorite idempotence','creator/admin deletion ACL','required admin reason and audit',
            'deleted old media/share/favorites denial','block revokes friends audience','public profile excludes private fields']}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('PASS real HTTP unified activities, profiles, privacy, favorites, media and administrator audit')


if __name__=='__main__':main()
