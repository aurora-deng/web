"""Existing isolated PostgreSQL and real local TTS; no cloud key or cloud request."""
from pathlib import Path
import argparse
import importlib.util
import io
import json
import math
import ssl
import struct
import sys
import time
import uuid
import wave

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('speech_archive_helpers',ROOT/'tests/function3/integration/private_archive_blackbox.py')
helpers=importlib.util.module_from_spec(spec);sys.modules[spec.name]=helpers;spec.loader.exec_module(helpers)

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert helpers.runtime.sql('SELECT current_database()')=='function3_runtime_test'
    ctx=ssl.create_default_context(cafile='/home/pikachu/phase11-data/function3/runtime/localhost.crt');ctx.set_alpn_protocols(['http/1.1'])
    admin=helpers.Browser(ctx);user=helpers.Browser(ctx);prefix='speech_'+uuid.uuid4().hex[:8];admin.login(prefix+'_admin');user.login(prefix+'_user')
    helpers.runtime.sql(f"INSERT INTO site_roles VALUES({int(admin.user)},'administrator') ON CONFLICT(user_id) DO UPDATE SET role='administrator'")
    user.call('GET','/api/admin/speech/settings',expected=403)
    settings=admin.call('GET','/api/admin/speech/settings');assert settings['available']
    assert not settings['runtime']['azureKeyConfigured'] and not settings['runtime']['geminiKeyConfigured'],'This no-cloud test requires no site keys'
    report={'cloudCalls':0,'realCloudVerified':False,'naturalnessVerified':False,'voices':[]}
    try:
        for voice,style in [('zf_001','calm'),('zm_009','narration-relaxed')]:
            profile={'voice':voice,'provider':'azure','style':style,'region':'eastasia','speed':'0.95','pitch':'-2','freeTierConfirmed':'true','dataUseConsent':'false'}
            user.call('PATCH','/api/admin/speech/settings',profile,403)
            admin.call('PATCH','/api/admin/speech/settings',profile)
            admin.call('PATCH','/api/admin/speech/settings',{**profile,'provider':'unexpected'},400)
            start=time.monotonic();preview=admin.call('POST','/api/admin/speech/preview',{'voice':voice});deadline=start+110
            while preview['state']=='pending' and time.monotonic()<deadline:
                time.sleep(.5);preview=admin.call('GET','/api/admin/speech/preview/'+voice)
            assert preview['state']=='ready',preview
            actual=admin.call('GET','/api/admin/speech/settings')['runtime'];assert actual['actualProvider']=='local' and actual['fallbackReason']=='missing-key',actual
            connection=helpers.http.client.HTTPSConnection('127.0.0.1',18444,context=ctx,timeout=20)
            connection.request('GET',preview['url'],headers={'Cookie':admin.cookie});response=connection.getresponse();audio=response.read();connection.close();assert response.status==200
            with wave.open(io.BytesIO(audio),'rb') as wav:
                assert wav.getnchannels()==1 and wav.getsampwidth()==2
                seconds=wav.getnframes()/wav.getframerate();samples=wav.readframes(wav.getnframes());rms=math.sqrt(sum(x*x for x, in struct.iter_unpack('<h',samples))/(len(samples)//2))
            assert 1<seconds<=60 and rms>50,(seconds,rms)
            a.output.parent.mkdir(parents=True,exist_ok=True);(a.output.parent/(voice+'-prosody-v2.wav')).write_bytes(audio)
            report['voices'].append({'voice':voice,'speed':.95,'seconds':round(seconds,3),'rms':round(rms,2),'prepareSeconds':round(time.monotonic()-start,3),'provider':'local','fallback':'missing-key'})
        assert int(helpers.runtime.sql(f"SELECT count(*) FROM site_audit WHERE actor_id={int(admin.user)} AND action='speech.configure'"))>=2
        assert helpers.runtime.sql('SELECT count(*) FROM speech_usage')=='0','No-key fallback must not spend cloud budget'
        report['passed']=True
    finally:
        for original in settings['profiles']:
            admin.call('PATCH','/api/admin/speech/settings',{k:str(v).lower() if isinstance(v,bool) else str(v) for k,v in original.items()})
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print('PASS speech role isolation, audited settings, real two-voice no-key fallback and valid non-silent WAV; listening remains manual')

if __name__=='__main__':main()
