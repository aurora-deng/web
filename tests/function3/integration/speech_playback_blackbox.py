"""Real local TTS, HTTP/1+H2 audio and admin credential ACL; existing _test DB only.

Uses disposable placeholder keys and never calls a cloud provider. The server must
point WEB_SPEECH_*_KEY_FILE at this validation's private site-keys directory.
"""
import argparse
import hashlib
import http.client
import importlib.util
import io
import json
import math
import re
from pathlib import Path
import ssl
import stat
import struct
import subprocess
import sys
import time
import uuid
import wave

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('speech_playback_helpers',ROOT/'tests/function3/integration/private_archive_blackbox.py')
helpers=importlib.util.module_from_spec(spec);sys.modules[spec.name]=helpers;spec.loader.exec_module(helpers)

def inspect_audio(data):
    with wave.open(io.BytesIO(data),'rb') as source:
        assert source.getnchannels()==1 and source.getsampwidth()==2
        seconds=source.getnframes()/source.getframerate();samples=source.readframes(source.getnframes())
    rms=math.sqrt(sum(n*n for n, in struct.iter_unpack('<h',samples))/(len(samples)//2))
    assert 1<seconds<60 and rms>50,(seconds,rms)
    return {'seconds':round(seconds,3),'rms':round(rms,2),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    review=Path('/home/pikachu/phase11-data/function3/validation/speech-fix')
    assert helpers.runtime.sql('SELECT current_database()')=='function3_runtime_test'
    ca='/home/pikachu/phase11-data/function3/runtime/localhost.crt'
    context=ssl.create_default_context(cafile=ca);context.set_alpn_protocols(['http/1.1'])
    admin=helpers.Browser(context);user=helpers.Browser(context);prefix='voicefix_'+uuid.uuid4().hex[:7]
    admin.login(prefix+'_admin');user.login(prefix+'_user')
    helpers.runtime.sql(f"INSERT INTO site_roles VALUES({int(admin.user)},'administrator') ON CONFLICT(user_id) DO UPDATE SET role='administrator'")
    key=review/'site-keys/azure.key';assert not key.exists(),'Use an isolated empty credential path'
    placeholder='placeholder-test-only-'+uuid.uuid4().hex
    fields={'provider':'azure','apiKey':placeholder,'removeKey':'false'}
    user.call('PATCH','/api/admin/speech/credentials',fields,403);assert not key.exists()
    helpers.Browser(context).call('PATCH','/api/admin/speech/credentials',fields,401)
    csrf=admin.csrf;admin.csrf='invalid-test-csrf'
    admin.call('PATCH','/api/admin/speech/credentials',fields,403);admin.csrf=csrf;assert not key.exists()
    admin.call('PATCH','/api/admin/speech/credentials',{'provider':'invalid','apiKey':placeholder},400)
    admin.call('PATCH','/api/admin/speech/credentials',{**fields,'apiKey':'invalid\nheader'},400)
    # Even an authenticated administrator cannot transmit a site key over HTTP.
    connection=http.client.HTTPConnection('127.0.0.1',18094,timeout=10)
    connection.request('PATCH','/api/admin/speech/credentials',json.dumps(fields),headers={'Cookie':admin.cookie,'Origin':'http://127.0.0.1:18094','X-CSRF-Token':admin.csrf,'Content-Type':'application/json'})
    response=connection.getresponse();response.read();assert response.status==403;connection.close();assert not key.exists()
    admin.call('PATCH','/api/admin/speech/credentials',fields)
    assert key.read_text()==placeholder and stat.S_IMODE(key.stat().st_mode)==0o600
    settings=admin.call('GET','/api/admin/speech/settings');assert settings['runtime']['azureKeyConfigured']
    assert placeholder not in json.dumps(settings)
    replacement=placeholder+'-replacement';admin.call('PATCH','/api/admin/speech/credentials',{**fields,'apiKey':replacement})
    assert key.read_text()==replacement
    admin.call('PATCH','/api/admin/speech/credentials',{'provider':'azure','apiKey':'','removeKey':'true'})
    admin.call('PATCH','/api/admin/speech/credentials',{'provider':'azure','apiKey':'','removeKey':'true'})
    assert not key.exists() and not admin.call('GET','/api/admin/speech/settings')['runtime']['azureKeyConfigured']
    audit=helpers.runtime.sql(f"SELECT string_agg(action||':'||reason,',') FROM site_audit WHERE actor_id={int(admin.user)} AND action LIKE 'speech.credential.%'")
    assert 'speech.credential.saved' in audit and placeholder not in audit
    report={'cloudCalls':0,'realCloudVerified':False,'naturalnessVerified':False,'formalDatabaseChanged':False,'credentials':{'administratorOnly':True,'httpsRequired':True,'ownerOnlyFile':True,'noEchoOrAuditSecret':True,'replaceAndRemove':True},'voices':[]}
    for voice in ('zf_001','zm_009'):
        state=user.call('POST','/api/me/speech/preview',{'voice':voice,'speed':'0.95'});deadline=time.monotonic()+135
        while state['state']=='pending' and time.monotonic()<deadline:
            time.sleep(.5);state=user.call('GET','/api/me/speech/preview/'+voice)
        assert state['state']=='ready',state
        connection=http.client.HTTPSConnection('127.0.0.1',18444,context=context,timeout=20)
        connection.request('GET',state['url'],headers={'Cookie':user.cookie});response=connection.getresponse();data=response.read();mime=response.getheader('Content-Type');connection.close()
        assert response.status==200 and mime=='audio/wav';measure=inspect_audio(data)
        output=review/(voice+'-h2.wav');headers=review/(voice+'-headers.txt')
        config='header = "Cookie: '+user.cookie+'"\n'
        result=subprocess.run(['curl','--noproxy','*','--cacert',ca,'--http2','--fail','--silent','--show-error','--max-time','30','--config','-','--dump-header',str(headers),'--output',str(output),'--write-out','%{http_version}','https://127.0.0.1:18444'+state['url']],input=config,text=True,capture_output=True,check=True)
        assert result.stdout=='2',result.stdout
        assert output.read_bytes()==data,'HTTP/1 and H2 audio differ'
        report['voices'].append({'voice':voice,'http1':True,'http2':True,**measure})
    # Inject only into the explicitly isolated cache, never the daily speech cache.
    cache=review/'cache';key_name=state['url'].rsplit('/',1)[-1]
    assert re.fullmatch(r'[0-9a-f]{64}',key_name)
    cached=cache/(key_name+'.wav');assert cached.is_file(),'Set isolated WEB_FUNCTION3_TTS_CACHE before running range checks'
    original=cached.read_bytes()
    with wave.open(io.BytesIO(original),'rb') as source:
        params=source.getparams();pcm=source.readframes(source.getnframes())
    output=io.BytesIO()
    with wave.open(output,'wb') as target:target.setparams(params);target.writeframes(pcm*7)
    large=output.getvalue();assert 1024*1024<len(large)<6*1024*1024
    try:
        cached.write_bytes(large);parts=[];offset=0
        rejected=review/'no-range.json'
        result=subprocess.run(['curl','--noproxy','*','--cacert',ca,'--http2','--silent','--show-error','--max-time','30','--config','-','--output',str(rejected),'--write-out','%{http_version}:%{http_code}','https://127.0.0.1:18444'+state['url']],input='header = "Cookie: '+user.cookie+'"\n',text=True,capture_output=True,check=True)
        assert result.stdout=='2:400' and 'Range' in json.loads(rejected.read_text())['message']
        while offset<len(large):
            file=review/'range.wav';headers=review/'range-headers.txt'
            config='header = "Cookie: '+user.cookie+'"\n'
            config+='header = "Range: bytes='+str(offset)+'-'+str(offset+512*1024-1)+'"\n'
            result=subprocess.run(['curl','--noproxy','*','--cacert',ca,'--http2','--fail','--silent','--show-error','--max-time','30','--config','-','--dump-header',str(headers),'--output',str(file),'--write-out','%{http_version}:%{http_code}','https://127.0.0.1:18444'+state['url']],input=config,text=True,capture_output=True,check=True)
            assert result.stdout=='2:206',result.stdout
            match=re.search(r'content-range:\s*bytes (\d+)-(\d+)/(\d+)',headers.read_text(),re.I);assert match
            start,end,total=map(int,match.groups());chunk=file.read_bytes()
            assert start==offset and total==len(large) and len(chunk)==end-start+1 and len(chunk)<=512*1024
            parts.append(chunk);offset=end+1
        assert b''.join(parts)==large
        report['largeH2Audio']={'bytes':len(large),'chunks':len(parts),'maxChunkBytes':max(map(len,parts)),'identicalReassembly':True,'isolatedCacheOnly':True,'rangeRequired':True}
    finally:cached.write_bytes(original)
    report['passed']=True
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n')
    print('PASS real two-voice TTS, identical HTTP/1+HTTP/2 WAV, HTTPS/admin credentials, file privacy and audit')

if __name__=='__main__':main()
