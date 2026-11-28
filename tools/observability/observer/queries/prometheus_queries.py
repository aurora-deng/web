"""Bounded fan-out for fixed dashboard queries; failed panels retain their state."""
from concurrent.futures import FIRST_COMPLETED,ThreadPoolExecutor,wait
import threading
import time
from tools.observability.observer.sources.sources import BackendError,PANELS

POOL=ThreadPoolExecutor(max_workers=4,thread_name_prefix='prometheus-panels')
SLOTS=threading.BoundedSemaphore(8)


def ranges(source,panels,start,end,step,diagnostics):
    step=max(15,step);result={key:[] for key in panels};pending={};remaining=iter(panels)
    deadline=time.monotonic()+3;exhausted=False
    def failed(key,message):
        diagnostics[key]={'status':'unavailable','error':message}
    while not exhausted or pending:
        while not exhausted and len(pending)<4:
            try:key=next(remaining)
            except StopIteration:exhausted=True;break
            if time.monotonic()>=deadline:
                failed(key,'批量查询达到三秒预算');continue
            if not SLOTS.acquire(blocking=False):
                failed(key,'Prometheus 查询繁忙，请稍后重试');continue
            try:
                future=POOL.submit(source.prom,'query_range',{'query':PANELS[key][2],
                    'start':start,'end':end,'step':step})
            except BaseException:
                SLOTS.release();raise
            future.add_done_callback(lambda _:SLOTS.release());pending[future]=key
        if not pending:continue
        finished,_=wait(pending,timeout=max(0,deadline-time.monotonic()),return_when=FIRST_COMPLETED)
        if not finished:
            for future,key in pending.items():
                future.cancel();failed(key,'批量查询达到三秒预算')
            for key in remaining:failed(key,'批量查询达到三秒预算')
            break
        for future in finished:
            key=pending.pop(future)
            try:result[key]=future.result()['result']
            except (BackendError,KeyError,TypeError):failed(key,'Prometheus 数据源不可用或响应无效')
    return step,result
