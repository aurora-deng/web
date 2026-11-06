"""Real ranking -> Outbox -> TTS -> authenticated audio -> one-use claim.

Existing function3_runtime_test only; creates its own accounts/posts via HTTP.
No production database writes, provider keys or cloud requests. Run with the
owned isolation service on 18094/18444 and the already installed local worker.
"""
import argparse
import concurrent.futures
import http.client
import importlib.util
import json
from pathlib import Path
import ssl
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('announcement_speech_helpers',ROOT/'tests/function3/integration/speech_playback_blackbox.py')
helpers=importlib.util.module_from_spec(spec);sys.modules[spec.name]=helpers;spec.loader.exec_module(helpers)
Browser=helpers.helpers.Browser


def wait_for(call, predicate, seconds=50):
    deadline=time.monotonic()+seconds
    while True:
        value=call()
        if predicate(value):return value
        if time.monotonic()>=deadline:raise AssertionError(('timeout',value))
        time.sleep(.5)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    assert helpers.helpers.runtime.sql('SELECT current_database()')=='function3_runtime_test'
    context=ssl.create_default_context(cafile='/home/pikachu/phase11-data/function3/runtime/localhost.crt');context.set_alpn_protocols(['http/1.1'])
    author=Browser(context);listener=Browser(context);prefix='annvoice_'+uuid.uuid4().hex[:8]
    author.login(prefix+'_author');listener.login(prefix+'_listener')
    author.call('PATCH','/api/me',{'displayName':'语音闭环验证'})
    category=author.call('GET','/api/tree/categories')[0]['id']
    # Rank version must already exist: the first settlement intentionally stays silent.
    previous=wait_for(lambda:author.call('GET','/api/tree/ranking'),lambda v:int(v['version'])>0)
    old=author.call('GET','/api/announcements');after=max([int(row['id']) for row in old],default=0)
    body={'requestId':prefix,'body':'仅用于隔离验证的公开果实，确认实际公告语音链路。','categoryId':category}
    fruit=author.call('POST','/api/tree/posts',body,201)
    listener.call('POST','/api/tree/posts/'+fruit['id']+'/like',{'liked':'true'})
    listener.call('POST','/api/tree/posts/'+fruit['id']+'/replies',{'requestId':prefix+'_reply','body':'验证排行变化与公告。'},201)
    rank=wait_for(lambda:author.call('GET','/api/tree/ranking'),lambda v:int(v['version'])>int(previous['version']) and any(e['postId']==fruit['id'] for e in v['entries']))
    rows=wait_for(lambda:listener.call('GET','/api/announcements?after='+str(after)),lambda rows:any(int(r['version'])>=int(rank['version']) for r in rows))
    announcement=next(r for r in rows if int(r['version'])>=int(rank['version']));identifier=announcement['id']
    Browser(context).call('GET',f'/api/announcements/{identifier}/audio',expected=401)
    listener.call('PATCH','/api/me/notification-settings',{'sound':'false','voice':'zf_001'})
    assert listener.call('POST',f'/api/announcements/{identifier}/claim',{})['claimed'] is False
    voices=[]
    for voice in ('zf_001','zm_009'):
        status=wait_for(lambda:listener.call('GET',f'/api/announcements/{identifier}/audio?voice={voice}'),lambda v:v['state']=='ready',120)
        connection=http.client.HTTPSConnection('127.0.0.1',18444,context=context,timeout=20)
        connection.request('GET',status['url'],headers={'Cookie':listener.cookie});response=connection.getresponse();audio=response.read();connection.close()
        assert response.status==200 and response.getheader('Content-Type')=='audio/wav'
        measure=helpers.inspect_audio(audio);voices.append({'voice':voice,**measure})
    listener.call('PATCH','/api/me/notification-settings',{'sound':'true','voice':'zm_009'})
    assert listener.call('GET','/api/me/notification-settings')=={'sound':True,'voice':'zm_009'}
    # Two tabs share the user's cookie, but each has its own transport connection.
    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        claims=list(pool.map(lambda _:listener.call('POST',f'/api/announcements/{identifier}/claim',{})['claimed'],range(2)))
    assert sorted(claims)==[False,True],claims
    assert listener.call('POST',f'/api/announcements/{identifier}/claim',{})['claimed'] is False
    query=f"SELECT event_type||':'||count(*) FROM function3_outbox WHERE aggregate_id={int(identifier)} AND event_type IN ('announcement.created','speech.requested','announcement.audio.ready') GROUP BY event_type ORDER BY event_type"
    events=helpers.helpers.runtime.sql(query).splitlines()
    assert events==['announcement.audio.ready:2','announcement.created:1','speech.requested:1'],events
    report={'passed':True,'path':'rank-change -> announcement -> outbox -> local TTS -> audio -> one-use claim','announcement':identifier,'rankVersion':rank['version'],'voices':voices,'outboxEvents':events,'soundOffDoesNotClaim':True,'soundPreferenceRoundTrip':True,'concurrentClaims':claims,'anonymousAudioRejected':True,'cloudCalls':0,'formalDatabaseChanged':False}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('PASS real ranked-post announcement, both local voices, sound preference and concurrent one-use playback')


if __name__=='__main__':main()
