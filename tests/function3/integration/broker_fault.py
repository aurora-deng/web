"""Pause only the marked private Broker, crash only the marked test app, recover accepted writes."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import uuid
from runtime_blackbox import BrowserClient, CONNECTION, PG, expect, sql, wait_for

root=Path('/home/pikachu/phase11-data/function3')
assert sql('SELECT current_database()')=='function3_runtime_test'
assert (root/'rocketmq/FUNCTION3_PRIVATE_BROKER').exists()
broker=int((root/'rocketmq/broker.pid').read_text())
assert str(root/'rocketmq') in (Path('/proc')/str(broker)/'cmdline').read_bytes().decode()
source=Path('/home/pikachu/function3-src');control=source/'scripts/function3/validation/run_server.py'
token=uuid.uuid4().hex[:10];a=BrowserClient('127.0.0.1',18094);a.register_and_login('f3fault_'+token,'Function3-test-only!')
sql(f"INSERT INTO site_roles VALUES({a.user_id},'administrator') ON CONFLICT(user_id) DO UPDATE SET role='administrator'")
category=expect(a,'GET','/api/tree/categories')[0]['id']
def publish(key):return expect(a,'POST','/api/tree/posts',{'requestId':token+'-'+key,'body':'Broker 故障期间仍持久保存的果实 '+key,'categoryId':category},201)
paused=False
try:
    os.kill(broker,signal.SIGSTOP);paused=True
    start=time.monotonic();first=publish('dead');second=publish('recover')
    assert time.monotonic()-start<5,'HTTP business waited for Broker'
    event=sql(f"SELECT id FROM function3_outbox WHERE aggregate_type='tree.post' AND aggregate_id={first['id']} ORDER BY id LIMIT 1")
    wait_for(lambda:sql('SELECT attempts FROM function3_outbox WHERE id='+event),lambda n:int(n)>=1,20)
    # Shorten the fault drill without changing production retry policy.
    # 只修改已经结束租约的 pending 事件；不能与正在运行的发布任务争改 attempts。
    wait_for(lambda:sql("UPDATE function3_outbox SET attempts=7,available_at=CURRENT_TIMESTAMP WHERE id="+event+" AND state='pending' AND attempts>=1 RETURNING id"),lambda value:value==event,45)
    wait_for(lambda:sql('SELECT state FROM function3_outbox WHERE id='+event),lambda s:s=='dead',60)
    assert expect(a,'GET','/api/tree/posts/'+first['id'])['body'].endswith('dead')
    expect(a,'POST','/api/admin/messaging/'+event+'/replay',{'reason':'隔离 Broker 故障演练'})
    # Crash after durable HTTP acceptance but before event delivery. PID and generation are checked.
    app=int((root/'validation/current.pid').read_text());process=Path('/proc')/str(app)
    assert str(source/'build-release/webserver') in (process/'cmdline').read_bytes().decode()
    assert (process/'stat').read_text().split()[21]==(root/'validation/current.ticks').read_text()
    os.kill(app,signal.SIGKILL)
    wait_for(lambda: not process.exists() or (process/'stat').read_text().split()[2]=='Z',bool,5)
    os.kill(broker,signal.SIGCONT);paused=False
    subprocess.run(['python3',str(control),'start','--with-mq','--tts'],check=True)
    assert publish('recover')['id']==second['id'],'restart changed the idempotent accepted write'
    wait_for(lambda:sql(f"SELECT count(*) FROM function3_outbox WHERE aggregate_type='tree.post' AND aggregate_id IN ({first['id']},{second['id']}) AND state<>'published'"),lambda s:s=='0',80)
    assert sql(f"SELECT count(*) FROM tree_posts WHERE author_id={a.user_id}")=='2'
    assert sql(f"SELECT count(*) FROM site_audit WHERE actor_id={a.user_id} AND action='replay'")=='1'
    print(json.dumps({'passed':True,'coverage':['Broker paused','HTTP accepted without waiting for MQ','retry/dead','audited replay','app SIGKILL','restart dedup','pending event recovery']},ensure_ascii=False))
finally:
    if paused:os.kill(broker,signal.SIGCONT)
