import http.client
import base64
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
import json
from pathlib import Path
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.observability.laboratory.alert_receiver import Receiver
from tools.observability.observer.contracts.events import Invalid
from tools.observability.diagnostics.function_trace import FunctionRun, demo_workload
from tools.observability.observer.alerts.custom_alerts import advance, inspect_series
from tools.observability.observer.metrics.local_metrics import (LocalMetrics, business_alerts, histogram_points,
    parse_metrics, response_ratio, total_difference)
from tools.observability.observer.traces.local_traces import TraceIndex
from tools.observability.observer.resources.linux_resources import cpu_stat, paired, process_rates, process_stat, proportions, TICKS
from tools.observability.observer.resources.resource_history import DETAIL_SAMPLES, ResourceHistory
from tools.observability.observer.metrics.measurement_quality import empty_diagnostic
from tools.observability.observer.logs.log_store import LogStore
from tools.observability.observer.logs.structured_logging import LogTail,DiagnosticLogger,prune_log_files
from tools.observability.diagnostics.profile_formats import callgrind, dhat, syscalls
from tools.observability.observer.analysis.projection import agent_summary, device_summary, compare, trace_view
from tools.observability.observer.http.server import ObserverServer
from tools.observability.observer.metrics.service_levels import capture_counters, report
from tools.observability.observer.sources.sources import BackendError, NoTrace, PANELS, Sources
from tools.observability.observer.storage.store import Conflict, Store, WEEK_SECONDS
from tools.observability.observer.configuration.workspace import provision_files
from tools.observability.laboratory.scenarios import agent, dag, device, span


class ExternalFixture:
    trace_result = None
    fail = False

    def trace(self, trace_id):
        if self.fail:
            raise BackendError("unavailable")
        if self.trace_result is None:
            raise NoTrace
        return self.trace_result

    def prom(self, action, args=None):
        if action == "query_range":
            return {"result": [{"metric": {}, "values": [[1, "0"], [2, "NaN"]]}]}
        if action == "query":
            return {"result": [{"value": [1, "0"]}]}
        return {"alerts": []}


class LocalMetricContract(unittest.TestCase):
    def test_sqlite_write_gate_bounds_wait_without_blocking_readers(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "observer.sqlite")

            def second_writer():
                with store.connect():
                    pass

            with store.connect() as writer:
                with patch("tools.observability.observer.storage.store.SQLITE_WAIT_SECONDS", 0):
                    with ThreadPoolExecutor(max_workers=1) as worker:
                        with self.assertRaisesRegex(sqlite3.OperationalError, "database is locked"):
                            worker.submit(second_writer).result(timeout=3)
                writer.execute("INSERT INTO runs(digest,started_ns,received_ns,body) VALUES ('check',1,1,'{}')")
                with store.read() as reader:
                    self.assertEqual(reader.execute("SELECT count(*) FROM runs").fetchone()[0], 0)
            second_writer()
            with store.read() as reader:
                self.assertEqual(reader.execute("SELECT count(*) FROM runs").fetchone()[0], 1)

    def test_fixed_business_catalog_and_outcome_metrics_do_not_turn_absence_into_zero(self):
        from tools.observability.observer.contracts.operation_catalog import FUNCTION_DEFINITIONS,FUNCTION_CALLS,FUNCTION_IN_FLIGHT
        from tools.observability.observer.metrics.measurement_quality import empty_diagnostic
        self.assertEqual(len(FUNCTION_DEFINITIONS),29)
        self.assertEqual(len({row['symbol'] for row in FUNCTION_DEFINITIONS}),29)
        preparation = {row['key']: row['symbol'] for row in FUNCTION_DEFINITIONS
                       if row['key'].startswith('ai_prepare_')}
        self.assertEqual(preparation, {
            'ai_prepare_audit': 'AiChatService::prepareAudit',
            'ai_prepare_history': 'AiChatService::prepareHistory',
            'ai_prepare_rag': 'AiChatService::prepareRag',
        })
        body=(FUNCTION_CALLS+'{function="TreeService::list",outcome="error"} 0\n'+
              FUNCTION_IN_FLIGHT+'{function="TreeService::list"} 0\n').encode()
        captured=parse_metrics(body)
        self.assertEqual(len(captured),2)
        now=int(time.time())
        source=LocalMetrics(65535,'',None,lambda _:None)
        source.rows.append((now,{'function_tree_list_rate':0,'_metrics':{'status':200,
            'histograms':['nebula_function_seconds'],'functions':['ChatService::conversations'],'functionOutcomes':False}}))
        diagnostic=empty_diagnostic('function_tree_list_p99',list(source.rows),now,0)
        self.assertEqual(diagnostic['reason'],'missing_function')
        source.rows[-1][1]['_metrics']['functions'].append('TreeService::list')
        self.assertEqual(empty_diagnostic('function_tree_list_error_rate',list(source.rows),now,0)['reason'],'missing_outcomes')
        self.assertEqual(empty_diagnostic('function_tree_list_p99',list(source.rows),now,0)['reason'],'no_traffic')
        source.rows[-1][1].update(function_tree_list_error_rate=0,function_tree_list_in_flight=0)
        _,result=source.ranges(['function_tree_list_error_rate','function_tree_list_in_flight'],now-10,now,5)
        self.assertTrue(all(item[0]['values'][0][1]=='0' for item in result.values()))

    def test_empty_diagnostics_follow_actual_capture_and_keep_zero_meaningful(self):
        missing={"_reliability":{"scrape":True},"_metrics":{"status":200,"histograms":[]}}
        idle={"_reliability":{"scrape":True},"_metrics":{"status":200,
              "histograms":["nebula_http_handler_seconds"]},"http_rate":0}
        reason=lambda rows,end=100,step=5:empty_diagnostic("http_p99",rows,end,1,step)["reason"]
        self.assertEqual(reason([(100,missing)]),"missing_metric")
        self.assertEqual(reason([(100,idle)]),"no_traffic")
        self.assertEqual(reason([(100,{"_reliability":{"scrape":False},
                                     "_metrics":{"status":401}})]),"scrape_denied")
        self.assertEqual(reason([(100,{"_reliability":{"scrape":False},
                                     "_metrics":{"status":0}})]),"scrape_failed")
        self.assertEqual(reason([(80,idle)]),"stale")
        self.assertEqual(reason([(80,idle)],300,300),"no_traffic")
        self.assertEqual(reason([]),"no_samples")
        self.assertEqual(empty_diagnostic("http_p99",[],100,101)["reason"],"expired")

    def test_function_recorder_omits_inconsistent_cpu_reading(self):
        run = FunctionRun(__file__,set())
        with patch.object(time,"perf_counter_ns",return_value=6_000_000), patch.object(
                time,"thread_time_ns",side_effect=(3_000_000,16_000_000)):
            for name in ("valid","coarse_clock"):
                run.record(name,secrets.token_hex(8),None,(1_000_000_000,1_000_000,0),1)
        first, second = run.spans
        self.assertEqual(first["attributes"][0]["value"]["intValue"],"3000000")
        self.assertNotIn("attributes",second)
        self.assertEqual(int(second["endTimeUnixNano"])-int(second["startTimeUnixNano"]),5_000_000)

    @staticmethod
    def reliability_rows(end, seconds=300):
        rows = []
        for index, stamp in enumerate(range(end-seconds,end+1,5)):
            count = index*20
            bad = max(0,index-(seconds-60)//5)*2
            rows.append((stamp,{"_instance":"127.0.0.1:18001",
                                "_reliability":{"epoch":"first","live":True,"ready":True,"scrape":True,
                                                "persisted":True,"series":{
                                    "adapter_http":[0,count-bad,0,0,bad],
                                    "conversation_latency":[count,count],
                                    "chat_ack":[0,0], "trace":[10*index,2*index,index]}}}))
        return rows

    def test_service_level_counts_and_multiwindow_gate(self):
        fixture = self.reliability_rows(10000)
        data = report(fixture,9940,10000,5)
        requests, latency, ack = data["objectives"]
        self.assertEqual((requests["observed"]["events"],requests["observed"]["bad"]),(240,24))
        self.assertAlmostEqual(requests["observed"]["burnRate"],10)
        self.assertEqual(requests["burns"][0]["state"],"within")
        self.assertAlmostEqual(requests["burns"][0]["long"]["burnRate"],2)
        self.assertIsNone(requests["budget"]["budgetRemainingFraction"])
        self.assertEqual(latency["observed"]["goodFraction"],1)
        self.assertEqual(ack["observed"]["state"],"no_traffic")
        self.assertIsNone(ack["observed"]["goodFraction"])
        self.assertEqual(data["telemetry"]["counts"],{"exported":120,"dropped":24,"queueFull":12})
        self.assertIsNone(data["httpTraceSampling"]["counts"])
        for i, (_, sample) in enumerate(fixture):
            sample["_reliability"]["series"]["adapter_http"]=[0,18*i,0,0,2*i]
            sample["_reliability"]["series"]["http_trace_sampling"]=[90*i,0]
        sampling = report(fixture,9940,10000,5)["httpTraceSampling"]
        self.assertEqual(sampling["counts"],{"skipped":1080,"failureRoots":0})
        self.assertEqual(sampling["state"],"data")
        fast, slow = report(fixture,9940,10000,5)["objectives"][0]["burns"]
        self.assertEqual((fast["state"], slow["state"]),("burning","insufficient"))
        complete = report(self.reliability_rows(30000,21600),29940,30000,5)["objectives"][0]["budget"]
        self.assertEqual((complete["state"],complete["events"]),("evaluated",86400))
        self.assertAlmostEqual(complete["budgetRemainingFraction"],1-(24/86400)/.01)

    def test_service_level_does_not_bridge_loss_reset_or_epoch(self):
        for reason in ("scrape_failed","before_capture","epoch_changed","reset","gap","inconsistent_bucket"):
            with self.subTest(reason=reason):
                series = self.reliability_rows(10000,60)
                head = series[-3][1]["_reliability"]
                if reason=="scrape_failed":
                    head["scrape"]=False
                elif reason=="before_capture":
                    series[-3][1].pop("_reliability")
                elif reason=="epoch_changed":
                    head["epoch"]="old"
                elif reason=="reset":
                    head["series"]["conversation_latency"]=[10_000,10_000]
                elif reason=="gap":
                    del series[-5:-2]
                else:
                    head["series"]["conversation_latency"][0]-=2
                reading = report(series,9940,10000,5)["objectives"][1]["observed"]
                self.assertEqual((reading["state"],reading["reason"]),("partial",reason))
                self.assertEqual(reading["events"],20)
        rows = self.reliability_rows(10000,60)
        data = report(rows,9940,10025,5)
        self.assertEqual(data["objectives"][1]["observed"]["reason"],"stale")
        self.assertIsNone(data["collecting"]["scrape"])

    def test_exact_slo_counter_capture(self):
        metrics = parse_metrics(b"""nebula_http_handler_seconds_count{route="conversations"} 9
nebula_http_handler_seconds_bucket{route="conversations",le="0.04"} 2
nebula_http_handler_seconds_bucket{route="conversations",le="0.08"} 7
nebula_chat_stage_seconds_bucket{stage="ack_ready",le="0.04"} 0
nebula_chat_stage_seconds_count{stage="ack_ready"} 0
nebula_telemetry_dropped_total 1
nebula_telemetry_exported_total 10
nebula_telemetry_queue_full_total 0
nebula_http_trace_sampling_skipped_total 25
nebula_http_trace_failure_roots_total 2
""")
        groups = capture_counters(metrics)
        self.assertEqual(groups,{"conversation_latency":(7,9),"chat_ack":(0,0),"trace":(10,1,0),
                                 "http_trace_sampling":(25,2)})

    def test_cpu_denominators_and_kernel_parsers(self):
        sample="23 (server name)) "+" ".join(["R",*[str(i) for i in range(1,40)]])
        stat=process_stat(sample)
        self.assertEqual(stat["name"],"server name)")
        self.assertEqual(stat["start"],19)
        cpu=cpu_stat("cpu 30 0 10 60 0 0 0 0\ncpu0 30 0 10 60 0 0 0 0\n")
        split,total=proportions(cpu["cpu"],dict.fromkeys(cpu["cpu"],0))
        self.assertEqual((split["system"],total),(10,100))
        result=process_rates({"userTicks":TICKS,"systemTicks":TICKS},{"userTicks":0,"systemTicks":0},2,4*TICKS,2*TICKS)
        self.assertEqual((result["cpuPct"],result["capacityPct"],result["busyPct"]),(100,50,100))
        self.assertNotIn("waitNsPerSec",result)
        self.assertEqual(paired("Tcp: InSegs RetransSegs\nTcp: 20 2\n")["Tcp:RetransSegs"],2)

    def test_profile_units_and_symbols(self):
        with tempfile.TemporaryDirectory() as work:
            path=Path(work)/"profile"
            path.write_text("events: Ir sysCount sysTime sysCpuTime\nfn=(1) service()\n1 12 1 50 30\ncfn=(3) worker()\ncalls=1 12\n2 500 0 0 0\n3 6\n",encoding="utf-8")
            functions=callgrind(path)
            self.assertEqual(functions["totalInstructions"],18)
            self.assertEqual(functions["syscallNs"][0]["value"],50)
            path.write_text(json.dumps({"ftbl":["[root]","allocate()"],"pps":[{"tb":80,"gb":40,"eb":8,"fs":[1]}]}))
            self.assertEqual(dhat(path)["retained"][0]["stack"],["allocate()"])
            path.write_text(" 80.00 0.018000 9000 2 epoll_wait\n 20.00 0.004000 1000 4 1 futex\n")
            self.assertEqual(syscalls(path)[1]["errors"],1)

    def test_histogram_uses_new_samples_and_preserves_zero_rate(self):
        base = "nebula_http_handler_seconds"

        def scrape(first, total, duration):
            return parse_metrics((
                f'{base}_count{{route="conversations"}} {total}\n'
                f'{base}_sum{{route="conversations"}} {duration}\n'
                f'{base}_bucket{{route="conversations",le="0.010"}} {first}\n'
                f'{base}_bucket{{route="conversations",le="0.05"}} {total}\n'
                f'{base}_bucket{{route="conversations",le="+Inf"}} {total}\n').encode())

        previous = scrape(6, 10, .2)
        current = scrape(8, 14, .3)
        args = (5, base, {"route": "conversations"}, "http", (50, 99))
        stats = histogram_points(current, previous, *args)
        self.assertAlmostEqual(stats["http_avg"], 25)
        self.assertAlmostEqual(stats["http_p50"], 10)
        self.assertAlmostEqual(stats["http_p99"], 49.2)
        self.assertEqual(histogram_points(current, current, *args), {"http_rate": 0})
        self.assertEqual(histogram_points(previous, current, *args), {})
        self.assertIsNone(total_difference(0, 1))

    def test_explore_reads_bucket_windows_and_rejects_unknown_queries(self):
        source=LocalMetrics(65535,"",None,lambda _:None)
        source.recent_histograms.append((110,json.dumps({"seconds":299.4,"nebula_http_handler_seconds":{
            "conversations":[[0.01,9],[0.02,10],["+Inf",10]],"history":[[0.01,4],[0.02,4],["+Inf",4]]},
            "nebula_function_seconds":{"ChatService::conversations":[[0.01,9],[0.02,10],["+Inf",10]]}})))
        source.recent_histograms.append((115,"{}"))
        expr='histogram_quantile(0.99, sum by (le, route) (rate(nebula_http_handler_seconds_bucket[5m])))'
        result=source.histogram(expr,100,120,5)
        self.assertEqual([row["labels"]["route"] for row in result["series"]],["conversations","history"])
        detail=result["series"][0]
        self.assertAlmostEqual(detail["points"][0][1],0.019)
        self.assertEqual(detail["points"][1][1],None)
        self.assertEqual(detail["histogram"]["buckets"],[[.01,9],[.02,1],["+Inf",0]])
        query=PANELS["function_conversations_p95"][2]
        replay=source.prom("query_range",{"query":query,"start":100,"end":120,"step":5})
        self.assertAlmostEqual(float(replay["result"][0]["values"][0][1]),15.0)
        with self.assertRaises(Invalid):
            source.histogram("histogram_quantile(0.99, fake)",100,120,5)

    def test_alerts_require_same_response_scope_and_real_pending_duration(self):
        before=parse_metrics(b'nebula_business_http_responses_total{status_class="1xx"} 0\n'
                             b'nebula_business_http_responses_total{status_class="2xx"} 90\n'
                             b'nebula_business_http_responses_total{status_class="3xx"} 0\n'
                             b'nebula_business_http_responses_total{status_class="4xx"} 0\n'
                             b'nebula_business_http_responses_total{status_class="5xx"} 10')
        key2=("nebula_business_http_responses_total",(("status_class","2xx"),))
        key5=("nebula_business_http_responses_total",(("status_class","5xx"),))
        current={**before,key2:109,key5:12}
        self.assertAlmostEqual(response_ratio(current,before),200/21)
        self.assertIsNone(response_ratio(before,current))
        since={}
        metrics={"http_5xx_pct":200/21,"db_lifetime_max":2200}
        states=lambda when,value=metrics:[x["state"] for x in business_alerts(value,since,when)]
        self.assertEqual(states(100),["firing","pending"])
        self.assertEqual(states(399),["firing","pending"])
        self.assertEqual(states(400),["firing","firing"])
        self.assertEqual(states(401,{}),["no_data","no_data"])
        self.assertEqual(states(402),["firing","pending"])

    def test_custom_alert_only_advances_when_one_series_receives_fresh_samples(self):
        rule={"reducer":"avg","op":"gt","threshold":10,"pendingSeconds":10}
        def data(now,points):
            return inspect_series(rule,{"step":5,"series":[{"labels":{"instance":"A"},"points":points},
                {"labels":{"instance":"B"},"points":[[now,1]]}]},now)
        first,timers=advance(rule,data(100,[[95,20],[100,30]]),{},100,20)
        self.assertEqual((first["state"],first["value"]),("pending",25))
        still,timers=advance(rule,data(110,[[100,30]]),timers,110,20)
        self.assertEqual(still["state"],"pending")
        fired,timers=advance(rule,data(110,[[105,20],[110,40]]),timers,110,20)
        self.assertEqual(fired["state"],"firing")
        stale,timers=advance(rule,data(140,[[110,40]]),timers,140,20)
        self.assertEqual(stale["state"],"normal")
        self.assertEqual(timers,{})


class ObserverContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.store = Store(Path(self.temp.name) / "observer.sqlite")
        self.backend = ExternalFixture()
        self.token = secrets.token_urlsafe(32)
        self.server = ObserverServer(("127.0.0.1", 0), self.store, self.backend,
                                     [(self.token, "lab", "dag", True)])
        thread = threading.Thread(target=self.server.serve_forever)
        thread.start()

        def shutdown():
            self.server.shutdown()
            thread.join()
            self.server.server_close()
        self.addCleanup(shutdown)

    def http(self, route, data=None, headers=None):
        request = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        try:
            request.request("GET" if data is None else "POST", route, None if data is None else json.dumps(data),
                            dict(headers or {}))
            response = request.getresponse()
            return response.status, json.loads(response.read())
        finally:
            request.close()

    def submit(self, records, authorization=None):
        return self.http("/ingest/v1/events", records, {
            "Authorization": authorization or "Bearer " + self.token, "Content-Type": "application/json"})

    def test_executable_multi_agent_workflow_is_consumed_by_observer(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("The optional JavaScript workflow producer requires the existing Node runtime")
        root = Path(__file__).resolve().parents[2]
        for mode, decision in (("retry", "proposal"), ("stale", "hold")):
            result = subprocess.run([node, "examples/agent-coordination.mjs", mode], cwd=root,
                                    check=True, capture_output=True, encoding="utf-8", timeout=10)
            sample = json.loads(result.stdout)
            self.assertTrue(sample["simulated"])
            self.assertEqual(sample["result"]["state"], decision)
            records = sample["events"]
            trace = records[0]["traceId"]
            self.assertEqual(records[-1]["traceId"], trace)
            self.assertEqual(self.submit(records)[0], 200)
            code, observed = self.http("/api/v1/traces/" + trace)
            self.assertEqual(code, 200)
            self.assertEqual({row["eventId"] for row in observed["items"]}, {row["eventId"] for row in records})
            states = {(row["fields"].get("nodeId"), row["fields"].get("state"))
                      for row in observed["items"] if row["eventType"] == "dag.node"}
            if mode == "retry":
                self.assertIn(("observe", "retry"), states)
                self.assertIn(("review", "success"), states)
            else:
                self.assertIn(("plan", "skipped"), states)
                self.assertIn(("decide", "success"), states)

    def test_static_manifest_and_all_module_routes(self):
        digest = self.http("/health")[1]["assetVersion"]
        self.assertRegex(digest, "^[0-9a-f]{16}$")
        for path in ("/", "/app.js", "/workspace.js", "/dashboard.js", "/chart.js", "/explore.js",
                     "/custom_dashboards.js", "/dashboard_templates.js", "/distributed.js", "/trace_details.js",
                    "/resources.js", "/alert_rules.js", "/function_performance.js", "/function_traces.js",
                    "/reliability.js", "/query_editor.js", "/logs.js", "/style.css"):
            with self.subTest(path=path):
                conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
                try:
                    conn.request("GET", path)
                    response = conn.getresponse()
                    self.assertEqual(response.status, 200)
                    if path.endswith(".js"):
                        self.assertTrue(response.getheader("Content-Type").startswith("text/javascript"))
                    self.assertTrue(response.read())
                finally:
                    conn.close()

    def test_logs_require_scoped_auth_and_reject_private_fields(self):
        token=secrets.token_urlsafe(32)
        self.server.credentials.append((token,"business-logs","log",False))
        row={"schemaVersion":1,"recordId":"one","timeUnixNano":str(time.time_ns()),"level":"ERROR",
             "component":"business","code":"business.http.failed","traceId":"a"*32,"spanId":"b"*16,"durationUs":1234}
        headers={"Authorization":"Bearer "+token,"Content-Type":"application/json"}
        self.assertEqual(self.http("/ingest/v1/logs",[row])[0],401)
        self.assertEqual(self.http("/ingest/v1/logs",[row],{"Authorization":"Bearer "+self.token})[0],403)
        self.assertEqual(self.http("/ingest/v1/events",[row],headers)[0],403)
        self.assertEqual(self.http("/ingest/v1/logs",[row],headers)[1],{"inserted":1,"duplicates":0})
        self.assertEqual(self.http("/ingest/v1/logs",[row],headers)[1],{"inserted":0,"duplicates":1})
        self.assertEqual(self.http("/ingest/v1/logs",[{**row,"durationUs":10}],headers)[0],409)
        self.assertEqual(self.http("/ingest/v1/logs",[{**row,"recordId":"private","message":"private password"}],headers)[0],400)
        for key in ("code","level","operation","errorCode","errorType"):
            self.assertEqual(self.http("/ingest/v1/logs",[{**row,key:[]}],headers)[0],400)
        now=int(time.time())
        query=f"/api/v1/logs?from={now-5}&to={now+1}&source=business-logs&trace={'a'*32}"
        data=self.http(query)[1]
        self.assertEqual(len(data["items"]),1)
        self.assertEqual(data["items"][0]["durationUs"],1234)
        self.assertNotIn("private password",json.dumps(data))

    def test_logs_pagination_budget_and_week_boundary(self):
        logs=LogStore(self.store,capacity=4)
        now=time.time_ns()
        def row(i,stamp):
            return {"schemaVersion":1,"recordId":str(i),"timeUnixNano":str(stamp),"level":"INFO",
                    "component":"business","code":"business.operation.completed"}
        with patch("tools.observability.observer.logs.log_store.time.time_ns",return_value=now):
            logs.append("lab",[row(i,now-i) for i in range(5)])
            first=logs.search(now-100,now,{"limit":"2","source":"lab"})
            second=logs.search(now-100,now,{"limit":"2","source":"lab","cursor":first["next"]})
            self.assertEqual([r["recordId"] for r in first["items"]+second["items"]],["0","1","2","3"])
            self.assertIsNone(second["next"])
            with self.assertRaises(Invalid):logs.search(now-100,now,{"cursor":"0:1"})
        with self.store.connect() as db:
            for i in (-1,0,1):
                r=row(10+i,now-self.store.retention+i)
                db.execute("INSERT INTO diagnostic_logs(source,record_id,stamp,level,component,code,body) VALUES(?,?,?,?,?,?,?)",
                           ("boundary",r["recordId"],int(r["timeUnixNano"]),"INFO","business",r["code"],json.dumps(r)))
        self.store.prune(now)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM diagnostic_logs WHERE source='boundary'").fetchone()[0],2)

    def test_log_tail_partial_rotation_and_nonstructured_text(self):
        path=Path(self.temp.name)/"service.log"
        row={"schemaVersion":1,"recordId":"tail-one","timeUnixNano":str(time.time_ns()),"level":"WARN",
             "component":"business","code":"business.http.rejected"}
        path.write_bytes(b"password=not-to-be-collected\n"+b"NEBULA_LOG "+json.dumps(row).encode())
        tail=LogTail(self.server.logs,[])
        tail.files=[("tailed",path,None,0)]
        try:
            tail.tick()
            with path.open("ab") as stream:stream.write(b"\n")
            tail.tick();tail.tick()
            now=time.time_ns();data=self.server.logs.search(now-10**10,now,{"source":"tailed"})
            self.assertEqual(len(data["items"]),1)
            path.write_text("NEBULA_LOG "+json.dumps({**row,"recordId":"tail-two"})+"\n")
            tail.tick()
            data=self.server.logs.search(now-10**10,time.time_ns(),{"source":"tailed"})
            self.assertEqual(len(data["items"]),2)
        finally:tail.close()

    def test_diagnostic_queue_is_bounded_and_does_not_wait_for_storage(self):
        started,release=threading.Event(),threading.Event()
        class SlowSink:
            def append(self,*_):started.set();release.wait(2)
        logger=DiagnosticLogger(SlowSink(),capacity=2)
        try:
            logger.emit("observer.internal_error")
            self.assertTrue(started.wait(1))
            began=time.monotonic()
            for _ in range(20):logger.emit("observer.internal_error")
            self.assertLess(time.monotonic()-began,.5)
            self.assertGreaterEqual(logger.dropped,18)
        finally:release.set();logger.close()

    def test_owned_log_files_rotate_without_deleting_unrelated_files(self):
        root=Path(self.temp.name)/"structured"
        root.mkdir()
        expired=root/"observer-1900-01-01.log";expired.write_text("expired")
        other=root/"unrelated.log";other.write_text("keep")
        prune_log_files(root,"observer")
        self.assertFalse(expired.exists());self.assertTrue(other.exists())
        logger=DiagnosticLogger(self.server.logs,output_dir=root)
        logger.emit("observer.internal_error")
        logger.close()
        files=list(root.glob("observer-????-??-??.log"))
        self.assertEqual(len(files),1)
        self.assertIn("NEBULA_LOG",files[0].read_text())

    def test_lab_private_publication_preserves_previous_value_on_failed_replace(self):
        from tools.observability.laboratory.lab import private_write
        path=Path(self.temp.name)/"session.json"
        path.write_text('{"nebulaPort":18000}')
        with patch("tools.observability.lab.os.replace",side_effect=OSError("injected replacement failure")):
            with self.assertRaises(OSError):private_write(path,'{"nebulaPort":18001}')
        self.assertEqual(json.loads(path.read_text())["nebulaPort"],18000)
        self.assertEqual(list(path.parent.glob("session.json.tmp-*")),[])
        private_write(path,'{"nebulaPort":18001}')
        self.assertEqual(json.loads(path.read_text())["nebulaPort"],18001)

    def test_exception_log_retains_type_and_line_without_private_exception_text(self):
        logger=DiagnosticLogger(self.server.logs)
        try:
            try:raise ValueError("private-password-never-log")
            except ValueError:logger.emit("observer.internal_error",requestId="f"*16)
        finally:logger.close()
        now=time.time_ns()
        data=self.server.logs.search(now-10**10,now,{"level":"ERROR"})
        record=next(r for r in data["items"] if r.get("errorType")=="ValueError")
        self.assertGreater(record["sourceLine"],0)
        self.assertEqual(record["sourceFile"],"tests/project/test_observability.py")
        self.assertNotIn("private-password-never-log",json.dumps(data))
        found=self.server.logs.search(now-10**10,now,{"q":"f"*16})["items"]
        self.assertEqual(len(found),1)
        self.assertEqual(found[0]["requestId"],"f"*16)

    def test_query_editor_instant_and_step_use_real_local_samples(self):
        source=LocalMetrics(65535,"",None,lambda _:None)
        now=int(time.time())
        for stamp,count in ((now-10,2),(now-5,4)):
            source.recent_histograms.append((stamp,json.dumps({"seconds":300,"nebula_function_seconds":{
                "ChatService::conversations":[[.01,count],[.02,count],["+Inf",count]]}})))
        self.server.target_monitor.builtin=source
        self.server.sources=source
        expr='histogram_quantile(0.99, sum by (le, function) (rate(nebula_function_seconds_bucket[5m])))'
        request={"from":now-30,"to":now,"source":"nebula","expr":expr,"queryMode":"promql","queryType":"instant","stepSeconds":10}
        status,result=self.workspace("query",request)
        self.assertEqual(status,200)
        self.assertEqual((result["queryType"],result["step"]),("instant",10))
        self.assertEqual(len(result["series"][0]["points"]),1)
        self.assertEqual(result["series"][0]["points"][0][0],now-5)
        self.assertEqual(self.workspace("query",{**request,"stepSeconds":True})[0],400)
        self.assertEqual(self.workspace("query",{**request,"stepSeconds":1})[0],400)
        self.assertEqual(self.workspace("query",{**request,"queryType":"unbounded"})[0],400)

    def test_trace_completion_creates_safe_linked_diagnostic(self):
        from tools.observability.observer.traces.local_traces import allowed_spans
        trace,ident="c"*32,"d"*16
        now=time.time_ns()
        payload={"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"nebula-webserver"}}]},
            "scopeSpans":[{"spans":[{"traceId":trace,"spanId":ident,"name":"WS chat.send","kind":2,
                "startTimeUnixNano":str(now-1250000),"endTimeUnixNano":str(now),"status":{"code":1}}]}]}]}
        allowed_spans(payload,now)
        self.server.logs.from_spans(payload)
        result=self.server.logs.search(now-2*10**9,now,{"trace":trace})
        self.assertEqual(len(result["items"]),1)
        self.assertEqual(result["items"][0]["durationUs"],1250)
        self.assertEqual(result["items"][0]["source"],"nebula-traces")

    def test_custom_board_lifecycle_and_source_authorization(self):
        fixture = self.source_fixture()
        src = {"name":"Probe","type":"http","url":fixture+"/health"}
        route = "/api/v1/workspace/source/test"
        self.assertEqual(self.http(route,src)[0],403)
        self.assertEqual(self.workspace("source/test",src,origin="http://invalid.example")[0],403)
        self.assertEqual(self.workspace("source/test",{**src,"url":"http://169.254.169.254/health"})[0],400)
        self.assertEqual(self.workspace("source/test",src)[1]["code"],200)
        status, saved = self.workspace("source/save",src)
        self.assertEqual(status,200)
        panel = {"source":saved["id"],"title":"Health","queries":[{"expr":"probe_success","alias":"up"}],"view":"stat"}
        status, board = self.workspace("board/save",{"title":"Board","panels":[panel]})
        self.assertEqual(status,200)
        self.assertEqual(self.workspace("source/delete",saved)[0],409)
        updated = self.workspace("board/save",{**board,"title":"Edited"})[1]
        self.assertEqual(updated["revision"],2)
        self.assertEqual(self.workspace("board/save",board)[0],409)
        self.assertEqual(self.http("/api/v1/workspace")[1]["boards"][0]["title"],"Edited")
        self.assertEqual(self.workspace("board/delete",updated)[0],200)
        self.assertEqual(self.workspace("source/delete",saved)[0],200)

    def test_custom_alert_preview_save_pause_and_protect_source(self):
        source=self.workspace("source/save",{"name":"Health","type":"http","url":self.source_fixture()+"/health"})[1]
        rule={"name":"Health below expected value","source":source["id"],"expr":"probe_success",
              "reducer":"last","op":"lt","threshold":2,"windowSeconds":30,"pendingSeconds":20,
              "enabled":True,"severity":"warning","unit":"","impact":"Investigate service."}
        now=int(time.time())
        reply={"step":5,"series":[{"labels":{"instance":"sample"},"points":[[now-5,1],[now,1]]}]}
        with patch.object(self.server.target_monitor,"query",return_value=reply):
            status,preview=self.workspace("alert/preview",rule)
        self.assertEqual((status,preview["state"],preview["value"]),(200,"pending",1))
        self.assertEqual(self.http("/api/v1/alert-rules")[1]["rules"],[])
        status,stored=self.workspace("alert/save",rule)
        self.assertEqual(status,200)
        self.assertEqual(self.workspace("alert/save",{**rule,"threshold":None})[0],400)
        self.assertEqual(self.workspace("source/delete",source)[0],409)
        status,paused=self.workspace("alert/save",{**stored,"enabled":False})
        self.assertEqual(status,200)
        self.assertEqual(self.http("/api/v1/alert-rules")[1]["rules"][0]["result"]["state"],"paused")
        self.assertEqual(self.workspace("alert/delete",stored)[0],409)
        self.assertEqual(self.workspace("alert/delete",paused)[0],200)
        self.assertEqual(self.workspace("source/delete",source)[0],200)

    def test_concurrent_alert_edits_with_diagnostic_writes_preserve_revisions(self):
        source=self.workspace('source/save',{'name':'Concurrent health','type':'http',
            'url':self.source_fixture()+'/health'})[1]
        base={'source':source['id'],'expr':'probe_success','reducer':'last','op':'lt',
            'threshold':2,'windowSeconds':30,'pendingSeconds':0,'enabled':False,
            'severity':'warning','unit':'','impact':'Concurrency contract'}
        def edit(index):
            rule={**base,'name':'Concurrent '+str(index)}
            for revision in range(1,7):
                self.server.logger.emit('observer.invalid_request','WARN')
                status,rule=self.workspace('alert/save',{**rule,'threshold':revision})
                self.assertEqual(status,200,rule)
                self.assertEqual(rule['revision'],revision)
            return rule['id']
        with ThreadPoolExecutor(max_workers=4) as workers:
            identifiers=set(workers.map(edit,range(4)))
        rows=self.http('/api/v1/alert-rules')[1]['rules']
        self.assertEqual({row['id'] for row in rows},identifiers)
        self.assertTrue(all(row['revision']==6 and row['result']['state']=='paused' for row in rows))

    def test_local_dashboard_provisioning_is_atomic_and_idempotent(self):
        configs=Path(self.temp.name)/"board-files"
        configs.mkdir()
        path=configs/"nebula.json"
        dashboard={"schema":"nebula-observer/1","title":"Provisioned board","panels":[
            {"source":"nebula","title":"QPS","queries":[{"expr":"http"}],"view":"line"}]}
        path.write_text(json.dumps(dashboard))
        self.assertEqual(provision_files(configs,self.server.workspace,True)[0]["status"],"created")
        self.assertEqual(self.server.workspace.list("board"),[])
        first=provision_files(configs,self.server.workspace)[0]
        self.assertEqual((first["status"],first["revision"]),("created",1))
        same=provision_files(configs,self.server.workspace)[0]
        self.assertEqual((same["status"],same["revision"]),("unchanged",1))
        path.write_text(json.dumps({**dashboard,"title":"Updated managed dashboard"}))
        self.assertEqual(provision_files(configs,self.server.workspace)[0]["revision"],2)
        record=self.server.workspace.get("board",first["id"])
        self.assertEqual(record["provisioned"],"nebula.json")
        self.assertEqual(self.workspace("board/save",{**record,"title":"Do not overwrite file"}), (409,{"error":"此仪表盘由本地文件管理，请修改 JSON 后同步"}))

    def test_authenticated_function_run_parentage_and_source_readback(self):
        self.server.traces=TraceIndex(self.store)
        producer="local-functions"
        token=secrets.token_urlsafe(32)
        self.server.credentials.append((token,producer,"function",False))
        run=FunctionRun(demo_workload.__code__.co_filename,{"demo_workload","calculate","check_order"})
        run.run(demo_workload,[])
        wrapped={"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":producer}}]},
            "scopeSpans":[{"spans":run.spans}]}]}
        route="/ingest/v1/function-traces"
        headers={"Content-Type":"application/json"}
        auth={**headers,"Authorization":"Bearer "+token}
        self.assertEqual(self.http(route,wrapped,headers)[0],401)
        self.assertEqual(self.http(route,wrapped,{**headers,"Authorization":"Bearer "+self.token})[0],403)
        status, response = self.http(route,wrapped,auth)
        self.assertEqual(status,200,response)
        detail=self.http("/api/v1/traces/"+run.trace_id)[1]
        rows={row["name"]:row for row in detail["spans"]}
        self.assertEqual(rows["calculate"]["parent"],rows["demo_workload"]["id"])
        for row in detail["spans"]:
            if row["cpuNs"] is not None:
                self.assertLessEqual(0,int(row["cpuNs"]))
                self.assertLessEqual(int(row["cpuNs"]),int(row["end"])-int(row["start"]))
        end=int(time.time())+1
        self.assertEqual(self.http(f"/api/v1/traces?q=calculate&sort=slowest&to={end}")[1]["items"][0]["traceId"],run.trace_id)
        sample=dag().events
        self.submit(sample)
        self.assertTrue(self.http(f"/api/v1/events?domain=dag&source=lab&to={end}")[1]["items"])
        self.assertEqual(self.http(f"/api/v1/events?domain=dag&source=unconnected&to={end}")[1]["items"],[])

    def source_fixture(self):
        seen = self.checked_queries = []

        class Fixture(BaseHTTPRequestHandler):
            def log_message(self,*_):
                pass

            def do_GET(self):
                route = urlsplit(self.path)
                args = parse_qs(route.query)
                replies = {
                    "/health": {"status":"alive"},
                    "/ready": {"status":"ready"},
                    "/api/v1/query": {"status":"success","data":{"resultType":"vector","result":[{"value":[1,"1"]}]}},
                    "/api/v1/label/namespace/values": {"status":"success","data":["a.prod"]},
                    "/api/v1/label/app/values": {"status":"success","data":["orders"]},
                    "/api/v1/label/instance/values": {"status":"success","data":["node-1"]},
                    "/api/v1/query_range": {"status":"success","data":{"resultType":"matrix","result":[
                        {"metric":{"namespace":"a.prod"},"values":[[1,"0"],[2,"NaN"]]}]}},
                    "/api/search":{"traces":[{"traceID":"3"*32,"durationMs":50}]},
                    "/api/v3/services":{"services":["checkout"]},
                    "/api/v2/traces/"+"3"*32:{"batches":[]},
                    "/loki/api/v1/labels":{"status":"success","data":["namespace"]},
                    "/loki/api/v1/query_range":{"status":"success","data":{"resultType":"streams","result":[
                        {"stream":{"namespace":"a.prod"},"values":[["1700000000000000000","error checkout "+"3"*32]]}]}},
                }
                if route.path.startswith("/api/v3/traces"):
                    ident=base64.b64encode(bytes.fromhex("3"*32)).decode()
                    replies[route.path]={"result":{"resourceSpans":[{"scopeSpans":[{"spans":[{
                        "traceId":ident,"spanId":"4"*16,"name":"checkout","startTimeUnixNano":"1000","endTimeUnixNano":"3000"
                    }]}]}]}}
                seen.append((route.path,args))
                value = replies.get(route.path)
                body = b"# TYPE custom_jobs gauge\ncustom_jobs{node=\"alpha\"} 7\n"
                if route.path != "/metrics":
                    body=json.dumps(value).encode()
                self.send_response(200 if value or route.path=="/metrics" else 404)
                self.send_header("Content-Length",str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                data=json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                seen.append((self.path,data))
                variables={"profileTypes":[{"ID":"cpu:cpu:nanoseconds:cpu:nanoseconds","sampleUnit":"nanoseconds"}]}
                if self.path.endswith("/SelectMergeStacktraces"):
                    variables={"flamegraph":{"total":"50","names":["total","foo","bar"],
                        "levels":[{"values":["0","50","0","0"]},{"values":["0","20","12","1","5","25","23","2"]}]}}
                body=json.dumps(variables).encode()
                self.send_response(200)
                self.send_header("Content-Length",str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        upstream=ThreadingHTTPServer(("127.0.0.1",0),Fixture)
        worker=threading.Thread(target=upstream.serve_forever)
        worker.start()
        def cleanup():
            upstream.shutdown()
            worker.join()
            upstream.server_close()
        self.addCleanup(cleanup)
        return f"http://127.0.0.1:{upstream.server_port}"

    def workspace(self,op,data,origin=None):
        headers={"Content-Type":"application/json","X-Observer-Workspace":"1"}
        if origin:
            headers["Origin"]=origin
        return self.http("/api/v1/workspace/"+op,data,headers)

    def test_data_source_protocols_and_resource_scope(self):
        fixture=self.source_fixture()
        def save(kind,suffix="",**extra):
            status,body=self.workspace("source/save",{"name":kind,"type":kind,"url":fixture+suffix,**extra})
            self.assertEqual(status,200,body)
            return body["id"]
        prom=save("prometheus")
        raw=save("metrics","/metrics",metrics=["custom_jobs"])
        trace=save("tempo")
        log=save("loki")
        query=self.workspace("query",{"source":prom,"expr":'up{namespace=~"${namespace}"}',"scope":{"namespace":"a.prod"}})[1]
        self.assertEqual(query["series"][0]["points"],[[1,0.0],[2,None]])
        self.assertEqual(self.workspace("labels",{"source":prom,"label":"namespace"})[1]["values"],["a.prod"])
        scoped=self.workspace("labels",{"source":prom,"label":"instance","scope":{"bc":"dev","app":"orders"}})[1]
        self.assertEqual(scoped["values"],["node-1"])
        sent=next(args["match[]"][0] for path,args in self.checked_queries if path=="/api/v1/label/instance/values")
        self.assertEqual(sent,'{bc="dev",app="orders"}')
        sent=next(args["query"][0] for path,args in self.checked_queries if path=="/api/v1/query_range")
        self.assertEqual(sent,'up{namespace=~"a\\\\.prod"}')
        config=self.server.workspace.get("source",raw)
        self.server.target_monitor.collector(config)
        rows=self.workspace("query",{"source":raw,"expr":"custom_jobs"})[1]["series"]
        self.assertEqual(rows[0]["points"][-1][1],7)
        self.assertEqual(self.workspace("investigate",{"source":trace,"expr":"{ status = error }"})[1]["traces"][0]["durationMs"],50)
        lines=self.workspace("investigate",{"source":log,"expr":'{namespace="a.prod"}',"trace":"3"*32})[1]["logs"]
        self.assertEqual(lines[0]["labels"]["namespace"],"a.prod")
        sent=next(args["query"][0] for path,args in self.checked_queries if path=="/loki/api/v1/query_range")
        self.assertTrue(sent.endswith(' |= "'+"3"*32+'"'))

    def test_pyroscope_flamegraph_and_jaeger_v3(self):
        fixture=self.source_fixture()
        def save(kind):
            status,body=self.workspace("source/save",{"name":kind,"type":kind,"url":fixture})
            self.assertEqual(status,200,body)
            return body["id"]
        pyro,jaeger=save("pyroscope"),save("jaeger")
        kind=self.workspace("profile",{"source":pyro,"action":"catalog"})[1]["types"][0]["id"]
        profile=self.workspace("profile",{"source":pyro,"profileType":kind,"selector":'{service_name="checkout"}'})[1]
        self.assertEqual((profile["unit"],profile["total"],profile["levels"][1][1]["x"]),("nanoseconds",50,25))
        by_id=self.workspace("investigate",{"source":jaeger,"trace":"3"*32})[1]
        self.assertEqual(by_id["resourceSpans"][0]["scopeSpans"][0]["spans"][0]["traceId"],"3"*32)
        found=self.workspace("investigate",{"source":jaeger,"expr":"checkout"})[1]
        self.assertEqual(found["traces"][0]["rootTraceName"],"checkout")
        self.assertEqual(next(q["query.serviceName"][0] for p,q in self.checked_queries if p=="/api/v3/traces"),"checkout")

    def test_resource_report_import_and_time_window(self):
        report=Path(self.temp.name)/"detail.json"
        breakdown=Path(self.temp.name)/"ack.json"
        now=int(time.time())
        binary="a"*64
        report.write_text(json.dumps({"id":"test-lab","schema":"nebula-resource-lab/1","label":"test",
                                      "started":now-20,"finished":now-10,"binarySha256":binary,"runs":[{"mode":"pinned"}]}))
        self.assertEqual(self.server.resources.import_file(report),"test-lab")
        self.assertEqual(self.http("/api/v1/resources/reports")[1][0]["id"],"test-lab")
        self.assertEqual(self.http("/api/v1/resources/reports/test-lab")[1]["runs"][0]["mode"],"pinned")
        self.assertEqual(self.http("/api/v1/resources")[1]["status"],"empty")
        fields=("queueUs","poolAcquireUs","commitStartUs","commitUs","ackReadyUs")
        profile={"mode":"profile","passed":True,"instrumented":{"configuration":{"workers":16},
                 "durableChatAckMs":{"p99":120},"trace":{"serverUnits":"microseconds","paired":1200,
                 "serverQuantiles":{field:{"p99":1000} for field in fields},
                 "slowestOnePercent":[{"rttMs":120,"messageId":"private",**dict.fromkeys(fields,900)}]}}}
        breakdown.write_text(json.dumps({**profile,"binarySha256":"b"*64}))
        with self.assertRaises(Invalid):
            self.server.resources.import_file(report,breakdown)
        breakdown.write_text(json.dumps({**profile,"binarySha256":binary}))
        self.server.resources.import_file(report,breakdown)
        detail=self.http("/api/v1/resources/reports/test-lab")[1]["ackBreakdown"]
        self.assertEqual(detail["paired"],1200)
        self.assertEqual(detail["slowest"][0]["rttMs"],120)
        self.assertNotIn("messageId",detail["slowest"][0])

    def test_resource_detail_survives_fixed_window_and_compaction(self):
        stamp=int(time.time())-100
        cpu=cpu_stat("cpu 30 0 10 60 0 0 0 0\ncpu0 30 0 10 60 0 0 0 0\n")
        template={"host":"lab","kernel":"fixture","cpus":cpu,"counters":{},"gauges":{},
                  "processes":[],"schedstats":False}
        for i in range(DETAIL_SAMPLES+1):
            with patch("tools.observability.observer.resources.resource_history.snapshot",
                       return_value={**template,"at":stamp+i,"clock":float(i)}):
                self.server.resources.capture()
        oldest=self.server.resources.history(stamp,stamp)["latest"]
        self.assertEqual(oldest["logicalCpuCount"],1)
        self.assertEqual(list(oldest["cpu"]),["cpu"])
        previous=self.server.resources.history(stamp+1,stamp+DETAIL_SAMPLES-1)["latest"]
        self.assertIn("cpu0",previous["cpu"])

    def test_weekly_prune_keeps_boundary_and_compacts_history(self):
        LocalMetrics(18090,"",None,lambda _:None,self.store)
        TraceIndex(self.store)
        now=int(time.time())
        cutoff=now-WEEK_SECONDS
        bucket=(now-86400)//300*300
        with self.store.connect() as db:
            for table in ("metric_samples","histogram_windows","resource_snapshots"):
                for stamp in (cutoff-1,cutoff,bucket+1,bucket+2,now-5,now):
                    db.execute(f"INSERT INTO {table} VALUES (?,?)",(stamp,"{}"))
            db.executemany("INSERT INTO source_samples VALUES (?,?,'{}')",
                           [("a",cutoff-1),("a",bucket+1),("a",bucket+2),("b",bucket+1)])
            db.execute("INSERT INTO resource_profiles VALUES ('expired',?,'{}')",(cutoff-1,))
            db.execute("INSERT INTO alert_transitions(stamp,body) VALUES (?,'{}')",(cutoff-1,))
            db.execute("INSERT INTO runs(digest,started_ns,received_ns,body) VALUES ('old',?,?,'{}')",
                       ((cutoff-1)*10**9,now*10**9))
        self.server.store.prune(now*10**9)
        with self.store.connect() as db:
            for table in ("metric_samples","histogram_windows","resource_snapshots"):
                self.assertEqual([row[0] for row in db.execute(f"SELECT stamp FROM {table} ORDER BY stamp")],
                                 [cutoff,bucket+2,now-5,now])
            self.assertEqual([(row[0],row[1]) for row in db.execute("SELECT source,ts FROM source_samples ORDER BY source,ts")],
                             [("a",bucket+2),("b",bucket+1)])
            for table in ("resource_profiles","alert_transitions","runs"):
                self.assertEqual(db.execute(f"SELECT count(*) FROM {table}").fetchone()[0],0)

    def test_local_panel_batch_matches_single_panel(self):
        now=int(time.time())
        local=LocalMetrics(18090,"",None,lambda _:None)
        local.rows.extend([(now-10,{"http":2,"ws":0}),(now-5,{"http":4}),
                           (now,{"http":6,"ws":1})])
        self.server.sources=local
        bounds=f"from={now-15}&to={now}"
        status,batch=self.http(f"/api/v1/series/batch?{bounds}&panels=http,ws")
        self.assertEqual(status,200)
        self.assertTrue(self.http("/health")[1]["seriesBatch"])
        for key in ("http","ws"):
            status,single=self.http(f"/api/v1/series?{bounds}&panel={key}")
            self.assertEqual(status,200)
            actual=batch["panels"][key]
            for field in ("series","status","step","truncated"):
                self.assertEqual(actual[field],single[field])
        self.assertEqual(batch["panels"]["ws"]["series"][0]["points"],[[now-10,0],[now-5,None],[now,1]])
        for panels in ("", "http,http", "http,unknown"):
            self.assertEqual(self.http(f"/api/v1/series/batch?{bounds}&panels={panels}")[0],400)

    def test_empty_panel_explains_missing_instrumentation_without_inventing_latency(self):
        now=int(time.time())
        local=LocalMetrics(18090,"",None,lambda _:None)
        local.rows.append((now,{"http":0,"_reliability":{"scrape":True},
                              "_metrics":{"status":200,"histograms":[]}}))
        self.server.sources=local
        bounds=f"from={now-15}&to={now}"
        status,result=self.http(f"/api/v1/series/batch?{bounds}&panels=http,http_p99")
        self.assertEqual(status,200)
        self.assertEqual(result["panels"]["http"]["series"][0]["points"],[[now,0]])
        latency=result["panels"]["http_p99"]
        self.assertEqual((latency["status"],latency["series"]),("empty",[]))
        self.assertEqual(latency["diagnostic"]["reason"],"missing_metric")
        single=self.http(f"/api/v1/series?{bounds}&panel=http_p99")[1]
        self.assertEqual(single["diagnostic"],latency["diagnostic"])
        local.rows.clear()
        local.rows.append((now,{"http_rate":0,"_reliability":{"scrape":True},
                               "_metrics":{"status":200,"histograms":["nebula_http_handler_seconds"]}}))
        self.assertEqual(self.http(f"/api/v1/series?{bounds}&panel=http_p99")[1]["diagnostic"]["reason"],"no_traffic")

    def test_commit_report_archive_validates_pairing_and_hides_raw_identifiers(self):
        commit = Path(self.temp.name) / "commit.json"
        start = 10**16
        row = {"backendPid": 1234, "commitUs": 123000, "ackMs": 129, "queueUs": -1,
               "commitStartOffsetMs": 5, "pollsWithinCommit": 2, "backendWaitSamples": {"Timeout:PgSleep": 1},
               "diskDelta": {"8:0": {"observedMs": 12, "flushes": 0}}}
        observation = {
            "clock": "CLOCK_MONOTONIC", "workloadMonotonicNs": {"start": start, "end": start + 150_000_000},
            "commitsPaired": 2, "commitsWithBackendSample": 1, "slowCommits": 1, "slowCommitsWithBackendSample": 1,
            "slowCommitThresholdMs": 20, "slowCommitWaitSamples": {"Timeout:PgSleep": 1},
            "waitPolls": 2, "maxPollGapMs": 105, "sampleIntervalMs": 10, "sampleErrors": 0, "sampleLimitReached": False,
            "longestCommits": [row, {**row, "commitUs": 4000, "pollsWithinCommit": 0,
                                    "backendWaitSamples": {}, "diskDelta": {}}],
            "walDelta": None, "diskDevices": [{"device": "8:0", "name": "sda"}], "workloadDiskDelta": {},
            "postgresSettings": {"server_version_num": "180006", "fsync": "on", "synchronous_commit": "on",
                                 "track_wal_io_timing": "on", "wal_sync_method": "fdatasync"},
        }
        report = {
            "mode": "commit", "passed": True, "binarySha256": "b"*64, "existingDatabaseUsed": False,
            "commitControlDelayMs": 120, "extra": "secret-raw-data",
            "instrumented": {"configuration": {"workers": 1, "operations": 10},
                             "durableChatAckMs": {"p50": 90, "p95": 128, "p99": 129},
                             "durableChatAckSamples": 2,
                             "trace": {"serverUnits": "microseconds", "paired": 2,
                                       "serverQuantiles": {"commitUs": {"p50": 5000, "p95": 120000, "p99": 123000}}},
                             "commitObservation": observation},
        }
        commit.write_text(json.dumps(report), encoding="utf-8")
        self.assertEqual(self.server.resources.import_commit(commit, "commit-test", "延迟对照"), "commit-test")
        status, reports = self.http("/api/v1/resources/reports")
        self.assertEqual(status, 200)
        self.assertEqual((reports[0]["kind"], reports[0]["label"]), ("commit", "延迟对照"))
        status, detail = self.http("/api/v1/resources/reports/commit-test")
        self.assertEqual(status, 200)
        self.assertEqual(detail["durationMs"], 150)
        self.assertEqual(detail["slowWaitSamples"], {"Timeout:PgSleep": 1})
        self.assertIsNone(detail["longest"][0]["queueUs"])
        self.assertEqual(detail["longest"][0]["diskDelta"]["8:0"]["flushes"], 0)
        self.assertEqual(detail["longest"][1]["diskDelta"], {})
        for private_key in ("backendPid", "workloadMonotonicNs", "secret-raw-data"):
            self.assertNotIn(private_key, json.dumps(detail))
            self.assertNotIn(private_key.encode(), self.store.path.read_bytes())
        observation["slowCommitsWithBackendSample"] = 2
        commit.write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaises(Invalid):
            self.server.resources.import_commit(commit, "commit-test", "bad")
        self.assertEqual(self.http("/api/v1/resources/reports/commit-test")[1], detail)

    def test_trace_import_query_and_privacy(self):
        self.server.traces = TraceIndex(self.store)
        stamp, trace_id = time.time_ns()-2_000_000_000, secrets.token_hex(16)
        parent = span(trace_id, "probe GET /api/conversations", stamp, stamp+20_000_000)
        child = span(trace_id, "GET /api/conversations", stamp+2, stamp+10_000_000, parent=parent["spanId"], kind=2)
        child["attributes"] = [{"key":"input", "value":{"stringValue":"private-example"}},
                               {"key":"http.response.status_code","value":{"intValue":"200"}}]
        groups = [{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":service}}]},
                   "scopeSpans":[{"spans":[item]}]} for service,item in (("nebula-probe",parent),("nebula-webserver",child))]
        payload = {"resourceSpans":groups}
        headers = {"Content-Type":"application/json"}
        self.assertEqual(self.http("/v1/traces",payload,{**headers,"Origin":"https://invalid.example"})[0],403)
        self.assertEqual(self.http("/v1/traces",payload,headers)[0],200)
        self.assertEqual(self.http("/v1/traces",payload,headers)[0],200)
        start = int(time.time())-60
        self.assertEqual(self.http(f"/api/v1/traces?from={start}")[1]["items"][0]["traceId"],trace_id)
        self.assertEqual(self.http(f"/api/v1/traces?from={start}&failed=1")[1]["items"],[])
        self.assertEqual(self.http(f"/api/v1/traces?from={start}&statusClass=5xx")[1]["items"],[])
        failed = span(secrets.token_hex(16), "GET /api/conversations", stamp, stamp+2000, failed=True, kind=2)
        failed["attributes"]=[{"key":"http.response.status_code","value":{"intValue":"503"}}]
        self.server.traces.append({"resourceSpans":[{**groups[1],"scopeSpans":[{"spans":[failed]}]}]})
        self.assertEqual(self.http(f"/api/v1/traces?from={start}&statusClass=5xx")[1]["items"][0]["httpStatus"],503)
        view = self.http("/api/v1/traces/"+trace_id)[1]
        self.assertEqual(view["status"],"complete")
        self.assertEqual(len(view["spans"]),2)
        selected = next(item for item in view["spans"] if item["id"] == child["spanId"])
        self.assertEqual(selected["kind"],2)
        self.assertEqual(selected["attributes"],{"http.response.status_code":"200"})
        self.assertEqual(selected["resourceAttributes"],{"service.name":"nebula-webserver"})
        self.assertNotIn("private-example", self.store.path.read_bytes().decode("utf8",errors="ignore"))

    def test_function_lookup_returns_exact_span_not_root(self):
        self.server.traces = TraceIndex(self.store)
        stamp, trace_id = time.time_ns()-2_000_000_000, secrets.token_hex(16)
        root = span(trace_id, "GET /api/conversations/:id/messages", stamp, stamp+10_000_000, kind=2)
        target = span(trace_id, "ChatService::history", stamp+1_000_000, stamp+4_000_000, parent=root["spanId"])
        related = span(trace_id, "ChatService::historyBefore", stamp+5_000_000, stamp+7_000_000, parent=root["spanId"])

        def payload(service, spans):
            return {"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":service}}]},
                                      "scopeSpans":[{"spans":spans}]}]}

        self.server.traces.append(payload("nebula-webserver",[target,related]))
        url = "/api/v1/traces?function=ChatService%3A%3Ahistory"
        self.assertEqual(self.http("/api/v1/traces/"+trace_id)[1]["status"],"partial")
        self.server.traces.append(payload("nebula-webserver",[root]))
        items = self.http(url)[1]["items"]
        self.assertEqual([(row["spanId"],row["traceId"],row["durationMs"]) for row in items],
                         [(target["spanId"],trace_id,3.0)])
        self.assertEqual(self.http("/api/v1/traces/"+trace_id)[1]["status"],"complete")
        self.assertEqual(self.http("/api/v1/traces")[1]["items"][0]["spanId"],root["spanId"])
        self.assertEqual(self.http("/api/v1/traces?function=history")[0],400)

    def test_tempo_function_matches_preserve_span_duration(self):
        source = Sources("","http://127.0.0.1:13200")
        with patch.object(source,"fetch",return_value={"traces":[
            {"traceID":"f"*32,"durationMs":500,"startTimeUnixNano":"1000000000",
             "spanSets":[{"spans":[{"spanID":"b"*16,"startTimeUnixNano":"1050000000","durationNanos":"10000000"}]}]},
            {"traceID":"a"*32,"durationMs":300,"startTimeUnixNano":"2000000000"}]}) as fetch:
            rows = source.search_traces(0,3_000_000_000,"ChatService::history",False,10,service="nebula-webserver")
        self.assertEqual(rows[0]["spanId"],"b"*16)
        self.assertEqual(rows[0]["durationMs"],10)
        self.assertIsNone(rows[1]["durationMs"])
        self.assertIn('resource.service.name = "nebula-webserver"',fetch.call_args.args[2]["q"])

    def test_sampling_history_recovers_after_reopen(self):
        source = LocalMetrics(65535, "", None, lambda _:None, self.store)
        stamp = int(time.time())-2
        with self.store.connect() as db:
            db.execute("INSERT INTO metric_samples VALUES (?,?)",(stamp,'{"http": 1.75, "auth_rejected": 0}'))
        other = LocalMetrics(65535, "", None, lambda _:None, self.store)
        self.assertEqual(list(source.rows),[])
        self.assertEqual(list(other.rows),[(stamp,{"http":1.75, "auth_rejected":0})])

    def test_history_joins_the_persisted_prefix_to_the_recent_snapshot(self):
        source = LocalMetrics(65535, "", None, lambda _:None, self.store)
        cutoff = source.dense_floor()
        recent = {"http": 0, "_instance": "after-restart"}
        with self.store.connect() as db:
            db.executemany("INSERT INTO metric_samples VALUES (?,?)",
                           [(cutoff-1, '{"http":9}'), (cutoff, '{"http":0,"_instance":"after-restart"}')])
        source.rows.append((cutoff, recent))
        combined = source.metric_rows(cutoff-1, cutoff)
        self.assertEqual(combined, [(cutoff-1, {"http": 9}), (cutoff, recent)])
        self.assertIs(combined[-1][1], recent)
        self.assertEqual(source.metric_rows(cutoff-1, cutoff-1), combined[:1])

    def test_series_compression_negotiates_without_changing_data(self):
        now=int(time.time())
        source=LocalMetrics(18090,"",None,lambda _:None)
        source.rows.extend((stamp,{"http":stamp%13,"cpu":stamp%100}) for stamp in range(now-1200,now+1,5))
        self.server.sources=source
        route=f"/api/v1/series/batch?from={now-1200}&to={now}&panels=http,cpu"
        baseline=self.http(route)[1]["panels"]
        for accept,compressed in (("br, gzip", True), ("*;q=1, gzip;q=0", False)):
            with self.subTest(encoding=accept):
                request=http.client.HTTPConnection("127.0.0.1",self.server.server_port,timeout=3)
                try:
                    request.request("GET",route,headers={"Accept-Encoding":accept})
                    response=request.getresponse()
                    self.assertEqual(response.status,200)
                    self.assertEqual(response.getheader("Vary"),"Accept-Encoding")
                    self.assertEqual(response.getheader("Content-Encoding"),"gzip" if compressed else None)
                    body=response.read()
                    self.assertEqual(len(body),int(response.getheader("Content-Length")))
                    panels=json.loads(gzip.decompress(body) if compressed else body)["panels"]
                    for key in baseline:
                        self.assertEqual(panels[key]["series"],baseline[key]["series"])
                finally:
                    request.close()

    def test_service_level_endpoint_routes_only_supported_sources(self):
        end = int(time.time())
        params = f"?from={end-60}&to={end}"
        self.assertEqual(self.http("/api/v1/service-levels"+params)[1]["status"],"unsupported")
        source = LocalMetrics(65535,"",None,lambda _:None)
        source.rows.extend(LocalMetricContract.reliability_rows(end))
        self.server.sources = source
        status, result = self.http("/api/v1/service-levels"+params)
        self.assertEqual((status,result["source"],result["objectives"][0]["observed"]["events"]),(200,"local",240))

    def test_prometheus_service_levels_require_scope_for_two_instances(self):
        class StaticPrometheus(Sources):
            def prom(self, path, params):
                if params["query"] == 'up{job="nebula"}':
                    return {"result": [{"metric": {"instance": name}} for name in ("nebula-a", "nebula-b")]}
                if path != "query" or 'instance="nebula-b"' not in params["query"]:
                    raise BackendError("Wrong scope")
                return {"resultType": "matrix", "result": []}

        self.server.sources = StaticPrometheus("http://127.0.0.1", "http://127.0.0.1")
        end = int(time.time())
        path = f"/api/v1/service-levels?from={end-60}&to={end}"
        status, choices = self.http(path)
        self.assertEqual((status, choices["status"], choices["instances"]), (200, "choose_instance", ["nebula-a", "nebula-b"]))
        status, report = self.http(path+"&instance=nebula-b")
        self.assertEqual((status, report["instance"], report["source"]), (200, "nebula-b", "prometheus"))
        self.assertEqual(report["objectives"][0]["observed"]["state"], "no_data")

    def test_alert_receiver_http_firing_resolved(self):
        with Receiver(0) as receiver:
            worker = threading.Thread(target=receiver.serve_forever, daemon=True)
            worker.start()
            try:
                connection = http.client.HTTPConnection("127.0.0.1", receiver.server_port, timeout=2)
                try:
                    for status in ("firing", "resolved"):
                        payload = {"status": status, "alerts": [{"status": status, "labels": {
                            "alertname": "NebulaUnreachable", "instance": "nebula-a", "private": "omit"}}]}
                        connection.request("POST", "/webhook", json.dumps(payload))
                        response = connection.getresponse()
                        self.assertEqual(response.status, 200)
                        response.read()
                    connection.request("GET", "/notifications")
                    reply = connection.getresponse()
                    self.assertEqual(reply.status, 200)
                    notices = json.loads(reply.read())["notifications"]
                    self.assertEqual([row["status"] for row in notices], ["firing", "resolved"])
                    self.assertNotIn("private", notices[0]["alerts"][0]["labels"])
                finally:
                    connection.close()
            finally:
                receiver.shutdown()
                worker.join(timeout=3)

    def test_auth_allowlist_idempotency_and_rollback(self):
        fixture = dag()
        self.assertEqual(self.submit(fixture.events, "Basic " + self.token)[0], 401)
        dirty = {**fixture.events[0], "fields": {**fixture.events[0]["fields"], "prompt": "secret"}}
        self.assertEqual(self.submit([dirty])[0], 400)
        self.assertEqual(self.submit(fixture.events), (200, {"inserted":len(fixture.events),"duplicates":0}))
        self.assertEqual(self.submit(fixture.events)[1]["duplicates"], len(fixture.events))
        new = {**fixture.events[0], "eventId": "new"}
        wrong = {**fixture.events[0], "fields": {**fixture.events[0]["fields"],"state":"failed"}}
        self.assertEqual(self.submit([new, wrong])[0], 409)
        query = self.http("/api/v1/events?limit=200")[1]
        self.assertNotIn("new", [e["eventId"] for e in query["items"]])

    def test_partial_unavailable_and_complete_traces(self):
        fixture = dag()
        route = "/api/v1/traces/" + fixture.trace
        self.assertEqual(self.http(route)[0], 404)
        self.submit(fixture.events)
        self.assertEqual(self.http(route)[1]["status"], "partial")
        self.backend.fail = True
        self.assertEqual(self.http(route)[1]["status"], "unavailable")
        self.backend.fail = False
        self.backend.trace_result = {"trace":{"resourceSpans":[{"scopeSpans":[{"spans":fixture.spans}]}]}}
        view = self.http(route)[1]
        self.assertEqual(view["status"], "complete")
        self.assertEqual(len(next(s for s in view["spans"] if s["name"]=="D")["links"]), 2)

    def test_zero_and_missing_are_distinct(self):
        status, catalog = self.http('/api/v1/function-catalog')
        self.assertEqual(status, 200)
        self.assertTrue(catalog['fixedCardinality'])
        self.assertEqual(len(catalog['functions']), 29)
        keys = {row['key'] for row in catalog['functions']}
        self.assertTrue({'ai_prepare_audit', 'ai_prepare_history', 'ai_prepare_rag'} <= keys)
        self.assertIn('GET /api/tree/posts/:post', catalog['httpOperations'])
        status, report = self.http("/api/v1/series?panel=http")
        self.assertEqual(status, 200)
        self.assertEqual(report["series"][0]["points"], [[1, 0], [2, None]])
        self.assertEqual(self.http("/api/v1/series?panel=bad")[0], 400)
        self.assertEqual(self.http("/api/v1/catalog", headers={"Host": "remote.invalid"})[0], 403)

    def test_retention_projection_and_replay_scenarios(self):
        for kind, generator in (("agent",agent),("device",device)):
            fixture = generator()
            self.store.append(kind,kind,True,fixture.events)
        now = time.time_ns()
        events = self.store.events(0, now, 100)
        summary = agent_summary(events["items"])
        self.assertEqual(summary["skills"]["search"], {"finished":1,"succeeded":1})
        self.assertEqual(summary["attempts"], 2)
        self.assertEqual(summary["tokens"], {"inputTokens":120,"outputTokens":43})
        items = [e for e in events["items"] if e["eventType"].startswith("device.")]
        self.assertEqual(device_summary(items)[0]["states"], ["sent","ack","error","estop"])
        self.assertTrue(any(e["fields"].get("late",0) > 0 for e in items))
        self.store.prune(now + self.store.retention)
        self.assertFalse(self.store.events(0, now, 100)["items"])

    def test_compare_requires_identity_and_sufficient_samples(self):
        common = {k:"b"*64 for k in ("configSha256","workloadSha256","fixtureSha256","environmentSha256","binarySha256")}
        base = {"scenario":"mixed","identity":{**common,"commit":"a"*40},
                "result":{"workers":4,"ackSamples":1200,"errors":0,"ackP99Ms":10}}
        candidate = {**base,"result":{**base["result"],"ackP99Ms":12}}
        self.assertEqual(compare(base,candidate)["ackP99DeltaPercent"], 20)
        self.assertEqual(compare(base,{**candidate,"identity":{**candidate["identity"],"fixtureSha256":None}})["status"],"incomparable")


if __name__ == "__main__":
    unittest.main()
