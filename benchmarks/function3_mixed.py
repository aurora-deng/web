"""Fixed-client mixed HTTP/WS/SSE benchmark; record durable chat ACK separately."""
import argparse
import json
from pathlib import Path
import socket
import threading
import time
from phase15_business_benchmark import BusinessBenchmark, SseClient, ResourceSampler, percentile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',type=int,default=18094)
p.add_argument('--server-pid',type=int,required=True)
p.add_argument('--operations',type=int,default=1200)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--observe-database',choices=('function3_phase11_test','function3_baseline_test'))
args=p.parse_args();args.host='127.0.0.1';args.timeout=20;args.workers=4
b=BusinessBenchmark(args);pairs=[b.create_pair('f3mix'+str(i)) for i in range(args.workers)]
channels=b._message_channels(pairs);streams=[SseClient(pair.receiver.api) for pair in pairs]
stop=threading.Event();threads=[]
def drain(stream):
    while not stop.is_set():
        try:stream.read_event()
        except socket.timeout:continue
        except (OSError,AssertionError):return
for stream in streams:
    stream.connect();stream.socket.settimeout(.5)
    t=threading.Thread(target=drain,args=(stream,),daemon=True);t.start();threads.append(t)
# PushConsumer 组在旧实例退出后可能仍有重平衡窗口。用真实业务回环确认订阅就绪，
# 单独记录启动时间；不把旧实例的分配窗口混进稳定运行时的延迟样本。
ready=time.monotonic()
for i,(sender,receiver) in enumerate(channels):
    key=f'f3ready-{b.suffix}-{i}';sender.socket.settimeout(90);receiver.socket.settimeout(90)
    sender.send_json({'v':1,'type':'chat.send','id':key,'conversationId':pairs[i].conversation,'content':'订阅就绪探针'})
    sender.wait('chat.accepted',key);receiver.wait('chat.message',client_message_id=key)
    sender.socket.settimeout(args.timeout);receiver.socket.settimeout(args.timeout)
ready_seconds=time.monotonic()-ready
observer=None
if args.observe_database:
    from phase41_database_observer import DatabaseObserver
    observer=DatabaseObserver(args.observe_database)
metrics_before=b.application_metrics()
accepted=[];lock=threading.Lock();sampler=ResourceSampler(args.server_pid);sampler.start()
phases={}
def perform(worker,index):
    pair=pairs[worker];choice=index%10 # 固定操作比例不受线程抢到哪个任务影响。
    if choice<5:
        phases[worker]='http.conversations';status,data=pair.sender.api.request('GET','/api/conversations');assert status==200 and isinstance(data,list)
    elif choice<7:
        phases[worker]='http.history';status,data=pair.sender.api.request('GET',f'/api/conversations/{pair.conversation}/messages?after=0&limit=20');assert status==200 and isinstance(data,list)
    else:
        sender,receiver=channels[worker];key=f'f3mixed-{b.suffix}-{worker}-{index}';start=time.perf_counter()
        sender.send_json({'v':1,'type':'chat.send','id':key,'conversationId':pair.conversation,'content':'同配置混合负载：持久化 ACK 与在线送达分开计量'})
        phases[worker]='websocket.accepted:'+key;reply=sender.wait('chat.accepted',key);assert reply.get('status')=='inserted',reply
        with lock:accepted.append((time.perf_counter()-start)*1000)
        phases[worker]='websocket.delivery:'+key;receiver.wait('chat.message',client_message_id=key)
def operation(worker,index):
    try:perform(worker,index)
    except Exception as error:raise AssertionError(f'worker={worker} index={index} stage={phases.get(worker)}: {error}') from error
try:
    metric=b.run_parallel('mixed-http-ws-sse',args.workers,args.operations,operation)
    report={'subscriptionReadySeconds':round(ready_seconds,3),'configuration':{'workers':4,'operations':args.operations,'httpListPercent':50,'historyPercent':20,'chatPercent':30,'residentSse':4},
            'mixed':metric.summary(),'durableChatAckMs':{f'p{int(q*100)}':round(percentile(accepted,q),3) for q in (.5,.95,.99)},
            'durableChatAckSamples':len(accepted),'resources':sampler.stop(),'applicationMetrics':{'before':metrics_before,'after':b.application_metrics()}}
    if observer:report['databaseObservation']=observer.stop();observer=None
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False));assert metric.error_count==0,'mixed workload errors'
finally:
    if observer:observer.stop()
    stop.set()
    for stream in streams:stream.close()
    for sender,receiver in channels:sender.close();receiver.close()
    for user in b.users:user.api.close()
