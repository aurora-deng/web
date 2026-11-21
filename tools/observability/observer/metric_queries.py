"""Bounded metric queries and read projections, independent of HTTP transport."""
import math
import time
from .events import Invalid
from .sources import PANELS


def points(values):
    return [[stamp,value if math.isfinite(value:=float(raw)) else None] for stamp,raw in values]


def panel_series(series,step,query_ms,batch=False,diagnostic=None):
    normalized=[{'labels':row['metric'],'points':points(row['values'])} for row in series[:8]]
    present=any(value is not None for row in normalized for _,value in row['points'])
    result={'status':'ok' if present else 'empty','step':step,'queryMs':query_ms,
            'batch':batch,'truncated':len(series)>8,'series':normalized}
    if not present and diagnostic:
        result['diagnostic']=diagnostic
    return result


class MetricQueries:
    def __init__(self,sources):
        self.sources=sources

    def panel(self,panel,start,end):
        if panel not in PANELS:
            raise Invalid('未知面板')
        minimum=5 if getattr(self.sources,'profile',None)=='local' else 15
        step=max(minimum,math.ceil((end-start)/720/10**9))
        requested=time.monotonic()
        report=self.sources.prom('query_range',{'query':PANELS[panel][2],
            'start':start//10**9,'end':end//10**9,'step':step})
        return panel_series(report['result'],report.get('step',step),
            round((time.monotonic()-requested)*1000,2),diagnostic=report.get('diagnostic'))

    def batch(self,panels,start,end):
        if not 1<=len(panels)<=24 or len(set(panels))!=len(panels) or any(p not in PANELS for p in panels):
            raise Invalid('一次可查询 1 至 24 个不同的已登记面板')
        if not hasattr(self.sources,'ranges'):
            raise Invalid('此数据源使用单面板查询')
        step=max(5,math.ceil((end-start)/720/10**9));requested=time.monotonic();diagnostics={}
        actual,result=self.sources.ranges(panels,start//10**9,end//10**9,step,diagnostics)
        elapsed=round((time.monotonic()-requested)*1000,2)
        return {'panels':{key:({'status':'unavailable','series':[],'step':actual,'batch':True,
                              'queryMs':elapsed,'error':diagnostics[key]['error']}
                             if diagnostics.get(key,{}).get('status')=='unavailable' else
                             panel_series(result[key],actual,elapsed,batch=True,
                                         diagnostic=diagnostics.get(key))) for key in panels}}
