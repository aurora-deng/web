"""Verify original demo login, cache and real TTS; never creates users/posts or databases."""
import concurrent.futures
import hashlib
import http.client
import io
import json
from pathlib import Path
import struct
import time
import wave

root=Path('/home/pikachu/phase11-data/function3/runtime')
password=Path('/home/pikachu/phase11-data/secrets/phase11-demo.password').read_text().strip()
cookie='';csrf=''
def request(method,path,data=None,binary=False):
    conn=http.client.HTTPConnection('127.0.0.1',18090,timeout=8)
    headers={'Cookie':cookie,'Origin':'http://127.0.0.1:18090','X-CSRF-Token':csrf}
    body=None
    if data is not None:body=json.dumps(data).encode();headers['Content-Type']='application/json'
    conn.request(method,path,body,headers);response=conn.getresponse();raw=response.read();status=response.status;session=response.getheader('Set-Cookie');conn.close()
    if status>=400:raise RuntimeError(f'{method} {path}: HTTP {status}')
    return (raw if binary else json.loads(raw)),session

result,session=request('POST','/api/auth/login',{'username':'nebula_owner','password':password})
cookie=session.split(';',1)[0]
me,_=request('GET','/api/me');csrf=me['csrfToken']
assert me['user']['username']=='nebula_owner'
role,_=request('GET','/api/admin/me');assert role['role']=='administrator'
speech,_=request('GET','/api/me/speech/status');assert speech['enabled'] and speech['delivery']=='local'
for _ in range(12):request('GET','/api/tree/categories');request('GET','/api/tree/ranking')
cache,_=request('GET','/api/admin/cache');assert cache['enabled'] and cache['hits']>=10,cache
voices=[]
for voice in ('zf_001','zm_009'):
    start=time.monotonic();audio,_=request('POST','/api/me/speech/preview',{'voice':voice})
    deadline=time.monotonic()+120
    while audio['state']=='pending' and time.monotonic()<deadline:
        time.sleep(.7);audio,_=request('GET','/api/me/speech/preview/'+voice)
    assert audio['state']=='ready',audio
    raw,_=request('GET',audio['url'],binary=True)
    with wave.open(io.BytesIO(raw)) as wav:
        assert wav.getsampwidth()==2
        samples=wav.readframes(wav.getnframes());count=len(samples)//2
        rms=(sum(v*v for(v,)in struct.iter_unpack('<h',samples))/count)**.5
        seconds=wav.getnframes()/wav.getframerate()
        assert seconds>1 and rms>30,(seconds,rms)
    voices.append({'voice':voice,'bytes':len(raw),'seconds':round(seconds,3),'rms':round(rms,2),'sha256':hashlib.sha256(raw).hexdigest(),'readySeconds':round(time.monotonic()-start,3)})
report={'originalDemoLogin':True,'administrator':'nebula_owner','database':'phase11','speech':speech,'cache':cache,'voices':voices,'businessDataCreated':False}
(root/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
