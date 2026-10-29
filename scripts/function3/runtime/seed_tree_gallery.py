"""Add a small, labelled demo gallery through normal APIs; never impersonate users.

Idempotency keys are stable. No fake likes, friendships or ranking scores.
The dedicated demo account's random credential stays in a Linux-only 0600 file.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import secrets

STORIES=[
 ('清晨','把窗户推开，风里有一点桂花香。今天也从一件小事开始。'),
 ('学习','终于理清了一个困扰很久的问题，原来慢一点想清楚也是进步。'),
 ('散步','傍晚的云像铺开的粉色棉花，绕远一点回家也挺好。'),
 ('美食','第一次把面包烤成功了，边角有点焦，但香气很认真。'),
 ('音乐','循环了一首喜欢的歌。你有没有一首一听就想起某个季节的歌？'),
 ('旅行','想去看海，也想把平凡的周末过成小小的旅行。'),
 ('日常','整理完书桌，给自己泡一杯茶。把生活重新摆得舒服一点。'),
 ('心情','有些答案不用今天就找到。先照顾好自己，明天再继续。'),
 ('运动','今天又多走了一站路。慢慢积攒，也会到达很远的地方。'),
 ('阅读','在书里遇见了一句话：先把自己的生活过成喜欢的样子。'),
 ('花开','给树下的路人留一朵花。愿你的认真，被温柔地看见。'),
 ('相遇','这里是一枚演示果实。点击作者卡了解好友申请，也欢迎挂上真正属于你的故事。'),
]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',type=int,default=18090)
    p.add_argument('--apply',action='store_true',help='Explicitly create the demo account/posts')
    p.add_argument('--state-dir',type=Path,default=Path('/home/pikachu/phase11-data/function3/demo-gallery'))
    a=p.parse_args()
    if not a.apply:
        print(json.dumps({'dryRun':True,'posts':len(STORIES),'account':'tree_gallery_demo','port':a.port}));return
    if os.name!='posix' or not a.state_dir.resolve().is_relative_to(Path('/home/pikachu/phase11-data')):
        raise RuntimeError('State must stay under the authorised Linux data directory')
    a.state_dir.mkdir(parents=True,exist_ok=True,mode=0o700);os.chmod(a.state_dir,0o700)
    credentials=a.state_dir/'account.json'
    if credentials.exists():
        if credentials.stat().st_mode&0o077:raise RuntimeError('Credential permissions must be 0600')
        account=json.loads(credentials.read_text())
    else:
        account={'username':'tree_gallery_demo','password':secrets.token_urlsafe(30)}
        fd=os.open(credentials,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'w') as f:json.dump(account,f)
    cookie=csrf=''
    def call(method,path,data=None,accepted=(200,)):
        nonlocal cookie,csrf
        c=http.client.HTTPConnection('127.0.0.1',a.port,timeout=30)
        headers={'Content-Type':'application/json','Origin':'http://localhost'}
        if cookie:headers['Cookie']=cookie
        if csrf:headers['X-CSRF-Token']=csrf
        c.request(method,path,None if data is None else json.dumps(data).encode(),headers)
        r=c.getresponse();raw=r.read();status=r.status
        if value:=r.getheader('Set-Cookie'):cookie=value.split(';')[0]
        c.close();result=json.loads(raw) if raw else {}
        if status not in accepted:raise RuntimeError(f'{method} {path}: HTTP {status} (no credentials logged)')
        return result,status
    # A failed first run may already have registered the account; login remains authoritative.
    call('POST','/api/auth/register',{**account,'displayName':'演示园丁 · 示例内容'},(201,409))
    login,_=call('POST','/api/auth/login',account);csrf=login['csrfToken']
    call('PATCH','/api/me',{'displayName':'演示园丁 · 示例内容','biography':'这是用于展示果实造型和交互的演示账号，发布内容均为示例。'})
    categories,_=call('GET','/api/tree/categories');assert categories,'No active category'
    ids=[]
    for i,(tag,body) in enumerate(STORIES):
        post,_=call('POST','/api/tree/posts',{'requestId':f'gallery-v1-{i:02d}','body':'【演示内容】'+body,'categoryId':categories[0]['id'],'mood':'平静','tags':'演示\n'+tag},(201,))
        ids.append(post['id'])
    result={'accountId':str(login['user']['id']),'port':a.port,'postIds':ids,'label':'演示内容','count':len(ids)}
    (a.state_dir/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(result,ensure_ascii=False))

if __name__=='__main__':main()
