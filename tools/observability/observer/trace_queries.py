"""Trace read use cases. No HTTP handler, socket, response or UI dependencies."""
from dataclasses import dataclass
import time

from .events import Invalid, hex_id
from .local_traces import FUNCTION_SPANS, ROOT_OPERATIONS
from .projection import agent_cost, agent_summary, is_partial, trace_view
from .sources import BackendError, NoTrace


@dataclass(frozen=True)
class TraceSearchQuery:
    start: int
    end: int
    limit: int = 100
    operation: str = ''
    function: str = ''
    status_class: str = ''
    failed: bool = False
    keyword: str = ''
    minimum_ms: float = 0
    sort: str = 'recent'


class TraceQueries:
    def __init__(self, store, sources, index=None, pricing=None):
        self.store, self.sources, self.index, self.pricing = store, sources, index, pricing

    def search(self, query):
        q = query
        if q.operation and q.operation != 'probe GET /api/conversations' and q.operation not in ROOT_OPERATIONS:
            raise Invalid('未知链路操作')
        if q.function and (q.function not in FUNCTION_SPANS or q.operation):
            raise Invalid('函数链路查询只接受已埋点的完整函数名')
        if q.status_class and (q.status_class not in ('4xx','5xx') or not self.index):
            raise Invalid('HTTP 状态筛选需要本机链路索引及 4xx 或 5xx 状态大类')
        if len(q.keyword)>80 or not 0<=q.minimum_ms<=86400000 or q.sort not in ('recent','slowest'):
            raise Invalid('链路搜索条件无效')
        if self.index:
            items = self.index.search(q.start,q.end,q.function or q.operation,q.failed,q.limit,
                q.keyword,q.minimum_ms,q.sort=='slowest',service='nebula-webserver' if q.function else '',
                status_class=q.status_class)
        elif hasattr(self.sources,'search_traces'):
            items = self.sources.search_traces(q.start,q.end,q.function or q.operation,q.failed,q.limit,
                                             service='nebula-webserver' if q.function else '')
        else:
            items = []
        return {'items':items,'source':'local' if self.index else
                ('tempo' if hasattr(self.sources,'search_traces') else 'unconfigured')}

    def detail(self, trace_id):
        if not hex_id(trace_id,32):
            raise Invalid('traceId 无效')
        now = time.time_ns()
        evidence = self.store.events(now-self.store.retention,now+300*10**9,256,trace=trace_id)
        state, spans = 'complete', []
        try:
            raw = self.index.raw(trace_id) if self.index else self.sources.trace(trace_id)
            spans = trace_view(raw)
            if is_partial(raw,spans):
                state = 'partial'
        except NoTrace:
            state = 'partial' if evidence['items'] else 'missing'
        except (BackendError,KeyError,TypeError):
            state = 'unavailable'
        if (evidence['next'] or len(spans)>1000) and state=='complete':
            state='partial'
        return 404 if state=='missing' else 200, {'traceId':trace_id,'status':state,
            'source':'local' if self.index else 'tempo','spans':spans[:1000],
            'agent':agent_cost(agent_summary(evidence['items']),self.pricing),**evidence}
