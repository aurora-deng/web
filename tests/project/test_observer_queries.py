"""Real-backend query projection, bounded fan-out and partial failure contracts."""
from pathlib import Path
import sys
import threading
import time
import unittest
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT))
from tools.observability.observer.queries.metric_queries import MetricQueries
from tools.observability.observer.sources.sources import BackendError,Sources
from tools.observability.observer.sources.source_monitor import SourceMonitor,matrix_rows
from unittest.mock import patch


class Queries(unittest.TestCase):
    def test_ai_preparation_stages_have_fixed_bounded_queries(self):
        from tools.observability.observer.contracts.operation_catalog import FUNCTION_DEFINITIONS
        from tools.observability.observer.sources.sources import PANELS
        expected = {'ai_prepare_audit': 'AiChatService::prepareAudit',
                    'ai_prepare_history': 'AiChatService::prepareHistory',
                    'ai_prepare_rag': 'AiChatService::prepareRag'}
        catalog = {row['key']: row for row in FUNCTION_DEFINITIONS}
        self.assertEqual(len(catalog), 29)
        for key, symbol in expected.items():
            self.assertEqual(catalog[key]['domain'], 'ai')
            self.assertEqual(catalog[key]['symbol'], symbol)
            for suffix in ('p95', 'p99', 'rate', 'error_rate', 'in_flight'):
                query = PANELS[f'function_{key}_{suffix}'][2]
                self.assertIn(f'function="{symbol}"', query)
                self.assertNotIn('requestId', query)
                self.assertNotIn('conversationId', query)

    def test_batch_is_bounded_and_keeps_partial_failures(self):
        class Backend(Sources):
            def __init__(self):self.lock=threading.Lock();self.running=0;self.high=0
            def prom(self,path,params):
                with self.lock:self.running+=1;self.high=max(self.high,self.running)
                try:
                    time.sleep(.02)
                    if params['query']=='webserver_sse_online':raise BackendError('not available')
                    return {'result':[{'metric':{'instance':'fixture'},'values':[[1,'0']]}]}
                finally:
                    with self.lock:self.running-=1
        source=Backend();response=MetricQueries(source).batch(['ws','sse','queue','pool','http','cpu'],0,10**9)
        self.assertLessEqual(source.high,4);self.assertGreater(source.high,1)
        self.assertEqual(response['panels']['sse']['status'],'unavailable')
        self.assertEqual(response['panels']['ws']['status'],'ok')
        self.assertEqual(response['panels']['ws']['series'][0]['points'][0][1],0)
        self.assertEqual(response['panels']['ws']['step'],15)

    def test_prometheus_capability_is_not_an_invented_zero_call(self):
        class Backend(Sources):
            def __init__(self,rows):self.rows=rows
            def prom(self,*args):return {'result':self.rows}
        row=lambda name,**labels:{'metric':{'__name__':name,'instance':'fixture',**labels},'value':[123,'0']}
        result=Backend([row('nebula_function_seconds_count',function='ChatService::history'),
                        row('nebula_function_calls_total')]).instrumentation(0,123)
        self.assertTrue(result['available']);self.assertTrue(result['functionOutcomes'])
        self.assertEqual(result['observedFunctions'],['ChatService::history'])
        self.assertEqual(result['target']['instance'],'fixture')
        multiple=Backend([row('nebula_function_seconds_count',function='ChatService::history'),
                          {**row('nebula_function_seconds_count',function='ChatService::history'),
                           'metric':{'__name__':'nebula_function_seconds_count','instance':'other',
                                     'function':'ChatService::history'}}]).instrumentation(0,123)
        self.assertIsNone(multiple['target']['instance'])
        self.assertTrue(multiple['target']['multiple'])
        self.assertEqual(multiple['target']['instances'],['fixture','other'])
        self.assertFalse(Backend([]).instrumentation(0,123)['available'])

    def test_builtin_promql_uses_true_instant_endpoint(self):
        source=Sources('http://127.0.0.1:19091','http://127.0.0.1:13200')
        monitor=SourceMonitor(None,source,(),18798)
        with patch.object(source,'prom',return_value={'resultType':'vector',
            'result':[{'metric':{'function':'fixture'},'value':[100,'0']} ]}) as query:
            result=monitor.query({'source':'nebula','expr':'vector(0)','queryMode':'promql','queryType':'instant'},0,100)
            self.assertEqual(query.call_args.args[0],'query')
            self.assertEqual(query.call_args.args[1]['time'],100)
        self.assertEqual(result['status'],'ok')
        self.assertEqual(result['series'][0]['points'],[[100,0]])
        self.assertEqual(matrix_rows({'resultType':'scalar','result':[100,'2']},'instant'),[{'metric':{},'values':[[100,'2']]}])


if __name__=='__main__':unittest.main()
