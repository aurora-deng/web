"""One owned-lab ACK slowdown/recovery case using a real PostgreSQL row lock."""
import argparse
from contextlib import closing
from datetime import datetime,timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts/project'))
from repro_runtime import DATABASE,browser_fixture,require
from tools.observability.laboratory.scenarios import post, read_observer
from tools.observability.observer.storage.store import Store
from tools.observability.observer.http.access_preflight import prometheus_baseline


def sha(value):return hashlib.sha256(value.encode()).hexdigest()
def instant():return datetime.now(timezone.utc).isoformat().replace('+00:00','Z')


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--run-dir',type=Path,required=True)
    cli.add_argument('--postgres-prefix',type=Path,required=True)
    cli.add_argument('--output',type=Path,required=True)
    a=cli.parse_args();run=a.run_dir.resolve();session=json.loads((run/'session.json').read_text())
    require(session['profile'] in ('local','metrics') and session['nebulaPort']!=18090,'Use a disposable local lab')
    require(session.get('wsTraceEvery')==1,'This diagnostic requires --ws-trace-every 1')
    owner=int((run/'lab.pid').read_text());command=Path(f'/proc/{owner}/cmdline').read_bytes().split(b'\0')
    require(b'tools.observability.lab' in command and str(run).encode() in command,'Lab owner is not running')
    children=Path(f'/proc/{owner}/task/{owner}/children').read_text().split()
    server=next(int(pid) for pid in children if Path(f'/proc/{pid}/exe').resolve().name=='webserver')
    environment=dict(item.split(b'=',1) for item in Path(f'/proc/{server}/environ').read_bytes().split(b'\0') if b'=' in item)
    connection=environment[b'WEB_PHASE11_DATABASE'].decode()
    fields=dict(item.split('=',1) for item in connection.split())
    require(fields.get('dbname')==DATABASE and fields.get('host','').startswith('/tmp/nebula-pg-')
            and 'password' not in fields and 'hostaddr' not in fields,'Original or remote databases are excluded')
    psql=a.postgres_prefix.resolve()/'bin/psql'
    def sql(statement,env=None):
        return subprocess.check_output([str(psql),'-XqAt','-w','-d',connection,'-v','ON_ERROR_STOP=1','-c',statement],
            env=env,text=True,timeout=5).strip()
    require(sql('SELECT current_database()')==DATABASE,'Unexpected database')
    if session['profile']=='metrics':
        # A counter's first scrape is a baseline, not an observed increment.
        # Send the controlled workload only after this target has been scraped.
        prometheus_baseline(session['ports']['prometheus'],session['nebulaPort'])
    web_port = session['ports'].get('gateway')
    origin = f'http://127.0.0.1:{web_port}' if web_port else 'http://localhost'
    browser=browser_fixture(ROOT)
    clients=[browser.BrowserClient('127.0.0.1',session['nebulaPort'],origin=origin) for _ in range(2)]
    prefix='ackcase_'+secrets.token_hex(6);left,right=clients
    for index,client in enumerate(clients):client.register_and_login(prefix+str(index),secrets.token_urlsafe(24))
    def expect(client,method,path,body=None,code=200):
        status,value=client.request(method,path,body);require(status==code,'Fixture API rejected '+path);return value
    invitation=expect(left,'POST','/api/friend-requests',{'receiverId':str(right.user_id)},201)
    expect(right,'POST','/api/friend-requests/'+str(invitation['id']),{'accept':'true'})
    conversation=expect(left,'POST','/api/conversations',{'kind':'direct','peerId':str(right.user_id)},201)['id']
    require(str(conversation).isascii() and str(conversation).isdigit(),'Fixture ID must be decimal')
    base=f"http://127.0.0.1:{session['ports']['observer']}"
    producer=json.loads((run/'producers.json').read_text())['experiment-lab'];require(not producer['simulated'],'Use a real experiment producer')
    def marker(state):
        post(base,'/ingest/v1/events',[{'schemaVersion':1,'eventId':prefix+'-'+state,'eventType':'experiment.marker',
            'occurredAt':str(time.time_ns()),'fields':{'runId':prefix,'state':state,'scenario':'ack-row-lock'}}],producer['token'])
    channel,reader=left.open_websocket();timings={};started=instant();marker('started')
    with closing(channel):
        def send(key,expected='inserted'):
            begin=time.monotonic();channel.sendall(browser.masked_frame(json.dumps({'v':1,'type':'chat.send',
                'id':key,'conversationId':conversation,'content':'isolated diagnostic fixture'}).encode()))
            reply=browser.wait_for_ws(reader,'chat.accepted',100)
            require(reply['status']==expected,'ACK/idempotency result differs')
            return (time.monotonic()-begin)*1000
        timings['before']=[send(prefix+'-b'+str(i)) for i in range(20)]
        application=prefix+'-lock';marker('fault');fault_begin=int(time.time())
        statement=f'BEGIN; UPDATE conversations SET title=title WHERE id={conversation}; SELECT pg_sleep(1.5); COMMIT;'
        holder=subprocess.Popen([str(psql),'-XqAt','-w','-d',connection,'-v','ON_ERROR_STOP=1','-c',statement],
            env={**os.environ,'PGAPPNAME':application},stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            deadline=time.monotonic()+2
            while sql("SELECT count(*) FROM pg_stat_activity WHERE application_name='"+application+"' AND wait_event='PgSleep'")!='1':
                require(time.monotonic()<deadline,'Row-lock holder did not become ready');time.sleep(.02)
            timings['fault']=[send(prefix+'-slow')]
            require(holder.wait(timeout=5)==0,'Row-lock holder failed')
        finally:
            if holder.poll() is None:holder.terminate();holder.wait(timeout=5)
        require(timings['fault'][0]>500,'The fault did not cause a measurable wait')
        marker('recovered');timings['after']=[send(prefix+'-a'+str(i)) for i in range(20)]
        send(prefix+'-slow','duplicate')
    matching=int(sql("SELECT count(*) FROM messages WHERE conversation_id="+str(conversation)+" AND client_message_id='"+prefix+"-slow'"))
    require(matching==1,'Retry duplicated an acknowledged row')
    trace=None
    for _ in range(40):
        rows=read_observer(base,f'/api/v1/traces?from={fault_begin-1}&to={int(time.time())+1}&operation=WS%20chat.send&minMs=500&sort=slowest')['items']
        if rows:trace=rows[0]['traceId'];break
        time.sleep(.15)
    require(trace is not None,'Slow ACK has no collected server trace')
    detail=read_observer(base,'/api/v1/traces/'+trace)
    database=[span for span in detail['spans'] if span['name']=='PostgreSQL command']
    require(any(int(span['end'])-int(span['start'])>500_000_000 for span in database),'Trace does not explain the row-lock wait')
    marker('finished');finish=instant()
    binary=Path(f'/proc/{server}/exe').resolve();version=expect(left,'GET','/health/version')
    identity={'commit':None,'sourceFingerprint':version['sourceFingerprint'],'binarySha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'configSha256':sha('isolated-ws-trace-every-1'),'workloadSha256':sha('20-before-1-lock-20-after-one-retry'),
        'fixtureSha256':sha('synthetic-two-friends-direct-conversation'),'environmentSha256':sha(sys.platform)}
    runs=[];store=Store(run/'events.sqlite')
    for phase,values in timings.items():
        ordered=sorted(values)
        runs.append(store.import_run({'schemaVersion':1,'scenario':'ack-'+phase,'startedAt':started,'finishedAt':finish,
            'identity':identity,'result':{'operations':len(values),'ackSamples':len(values),'errors':0,'ackP50Ms':ordered[(len(ordered)-1)//2],
                'ackP99Ms':ordered[-1],'matchingRows':matching}}))
    report={'passed':True,'case':'postgres-row-lock','businessContent':'synthetic-owned-fixture','realDatabaseWait':True,
        'traceId':trace,'traceStatus':detail['status'],'ackMs':timings,'matchingRowsAfterRetry':matching,'runDigests':runs,
        'observer':base,'traceUrl':base+'/?view=traces&trace='+trace,'identity':identity,'capacityClaimed':False,
        'p99Method':'nearest-rank; small diagnostic samples, not a capacity benchmark'}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':True,'faultAckMs':timings['fault'][0],'matchingRows':matching,'traceUrl':report['traceUrl']}))


if __name__=='__main__':main()
