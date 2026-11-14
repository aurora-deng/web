import http.client
import json
from pathlib import Path
import secrets
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.observability.observer.events import Invalid
from tools.observability.observer.projection import agent_summary, device_summary, compare, trace_view
from tools.observability.observer.server import ObserverServer
from tools.observability.observer.sources import BackendError, NoTrace
from tools.observability.observer.store import Conflict, Store
from tools.observability.scenarios import agent, dag, device


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
