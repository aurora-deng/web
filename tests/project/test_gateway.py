"""Regression checks for the gateway log boundary and dashboard provisioning."""

import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.observability.gateway import render
from tools.observability.observer.events import Invalid
from tools.observability.observer.gateway_logs import GatewayTelemetry, normalize_access
from tools.observability.observer.log_store import LogStore
from tools.observability.observer.store import Store, DAY
from tools.observability.observer.workspace import Workspace, provision_files


REQUESTS = [f"{letter * 8}-{letter * 4}-{letter * 4}-{letter * 4}-{letter * 12}" for letter in "bcd"]


def access(**fields):
    return {"at": f"{time.time():.3f}", "requestId": REQUESTS[0], "route": "conversations", "status": "200",
            "durationUs": "35000", "elapsedMs": "35", "details": "via_upstream", "upstreamConnectUs": "0",
            "upstreamHeaderUs": "31000", "upstreamResponseUs": "32000", "traceId": "a" * 32, **fields}


class GatewayContract(unittest.TestCase):
    def setUp(self):
        temporary = ROOT / "build-function3/gateway-contracts"
        temporary.mkdir(parents=True, exist_ok=True)
        owned = tempfile.TemporaryDirectory(dir=temporary)
        self.addCleanup(owned.cleanup)
        self.root = Path(owned.name)

    def test_store_kinds_and_fast_failure_are_distinguishable(self):
        store = Store(self.root / "events.sqlite")
        log = self.root / "access.jsonl"
        with patch.object(GatewayTelemetry, "collect"):
            gateway = GatewayTelemetry(LogStore(store), log)
            self.addCleanup(gateway.close)
        entry = access()
        failure = access(requestId=REQUESTS[1], route="other", status="504", durationUs="-", elapsedMs="15000",
                         details="upstream_response_timeout", upstreamConnectUs="1000", upstreamHeaderUs="-",
                         upstreamResponseUs="-", traceId="-")
        stream = access(requestId=REQUESTS[2], route="sse", durationUs="30000000", traceId="-")
        with log.open("wb") as out:
            out.write((json.dumps(entry) + "\n" + json.dumps(failure) + "\n" + json.dumps(stream)).encode())
        gateway.index_batch()
        first = gateway.metrics()
        self.assertIn('nebula_gateway_http_seconds_count{stage="request"} 2', first)
        self.assertIn('nebula_gateway_http_seconds_count{stage="upstream_header"} 1', first)
        self.assertIn('nebula_gateway_access_total{route="other",status_class="5xx",origin="gateway"} 1', first)
        self.assertNotIn('route="sse"', first)
        with log.open("ab") as out:
            out.write(b"\n")
        gateway.index_batch()
        self.assertIn('nebula_gateway_access_total{route="sse",status_class="2xx",origin="upstream"} 1', gateway.metrics())
        start, end = time.time_ns() - DAY, time.time_ns() + 10**9
        items = gateway.logs.search(start, end, {"source": "nebula-gateway"})["items"]
        indexed = {item["requestId"]: item for item in items}
        self.assertEqual(indexed[REQUESTS[0]]["upstreamConnectUs"], 0)
        self.assertEqual(indexed[REQUESTS[0]]["traceId"], "a" * 32)
        self.assertEqual(indexed[REQUESTS[1]]["gatewayOutcome"], "upstream_timeout")
        self.assertEqual(indexed[REQUESTS[1]]["durationUs"], 15_000_000)
        self.assertNotIn("upstreamHeaderUs", indexed[REQUESTS[1]])
        self.assertNotIn(REQUESTS[2], indexed)

    def test_rejects_request_data_and_unbounded_labels(self):
        for row in (access(path="/api/personal-secret?token=private"),
                    access(route="/api/conversations/326443"), access(traceId="header-supplied-secret"),
                    access(at="20.000"), access(status="502, 200")):
            with self.assertRaises(Invalid):
                normalize_access(row, time.time_ns(), DAY)

    def test_protected_ports_fail_and_gateway_board_uses_registered_queries(self):
        with self.assertRaises(ValueError):
            render(18095, 19097, 18090, self.root / "access.jsonl")
        with self.assertRaises(ValueError):
            render(18095, 19097, 38100, self.root / "access.jsonl", https_port=18445)
        generated = render(18095, 19097, 38100, self.root / "access.jsonl", https_port=18445,
                           cert=self.root / "test.crt", key=self.root / "test.key")
        self.assertEqual(generated["admin"]["address"]["socket_address"]["address"], "127.0.0.1")
        self.assertEqual(len(generated["static_resources"]["listeners"]), 2)
        for listener in generated["static_resources"]["listeners"]:
            manager = listener["filter_chains"][0]["filters"][0]["typed_config"]
            routes = manager["route_config"]["virtual_hosts"][0]["routes"]
            upgrades = [route["name"] for route in routes
                        if route.get("route", {}).get("upgrade_configs", [{}])[0].get("enabled")]
            self.assertEqual(upgrades, ["ws"])
            self.assertEqual(routes[-1]["direct_response"]["status"], 403)
        boards = provision_files(ROOT / "config/observability/dashboards", Workspace(Store(self.root / "events.sqlite")))
        self.assertTrue(any(item["file"] == "nebula-gateway.json" for item in boards))

    def test_dual_gateway_health_policy_uses_only_distinct_owned_endpoints(self):
        path = self.root / "access.jsonl"
        single = render(18095, 19097, 38100, path)["static_resources"]["clusters"][0]
        self.assertNotIn("health_checks", single)
        for invalid in (18090, 19097, 38100):
            with self.subTest(port=invalid), self.assertRaises(ValueError):
                render(18095, 19097, 38100, path, replica_port=invalid)
        with self.assertRaises(ValueError):
            render(18095, 19097, 38100, path, owns_upstream=False, replica_port=38101)
        dual = render(18095, 19097, 38100, path, replica_port=38101)["static_resources"]["clusters"][0]
        endpoints = dual["load_assignment"]["endpoints"][0]["lb_endpoints"]
        self.assertEqual([node["endpoint"]["address"]["socket_address"] for node in endpoints], [
            {"address": "127.0.0.1", "port_value": 38100}, {"address": "127.0.0.1", "port_value": 38101},
        ])
        self.assertEqual(dual["health_checks"][0]["http_health_check"]["path"], "/health/ready")
        self.assertEqual(dual["common_lb_config"]["healthy_panic_threshold"]["value"], 0)
        self.assertEqual(dual["circuit_breakers"]["thresholds"][0]["max_retries"], 0)


if __name__ == "__main__":
    unittest.main()
