"""Regression checks for the gateway log boundary and dashboard provisioning."""

from argparse import Namespace
from ipaddress import IPv4Address
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from backend.study.AccountWorkspace import LabRunner
from backend.study.AccountWorkspace.GatewayConfig import add_workspace
from tools.observability.gateway import render
from tools.observability.observer.events import Invalid
from tools.observability.observer.gateway_logs import GatewayTelemetry, ROUTES, normalize_access
from tools.observability.observer.log_store import LogStore
from tools.observability.observer.store import Store, DAY
from tools.observability.observer.workspace import Workspace, provision_files


REQUESTS = [f"{letter * 8}-{letter * 4}-{letter * 4}-{letter * 4}-{letter * 12}" for letter in "bcd"]


def selected_route(config, listener_name, path):
    listeners = config["static_resources"]["listeners"]
    listener = next(item for item in listeners if item["name"] == listener_name)
    routes = listener["filter_chains"][0]["filters"][0]["typed_config"]["route_config"]["virtual_hosts"][0]["routes"]
    for route in routes:
        match = route["match"]
        segment = match.get("path_separated_prefix")
        if path == match.get("path") or path.startswith(match.get("prefix", "\0")) or (
                segment and (path == segment or path.startswith(segment + "/"))):
            return route
    raise AssertionError(path)


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

    def test_study_gateway_checks_daily_authority_without_binding_its_port(self):
        options = Namespace(https_port=None, certificate=None, private_key=None, listen=IPv4Address('127.0.0.1'),
                            public_origin=[], study_port=18772, gateway_port=18095, admin_port=19097,
                            upstream_port=18090, accounts_dir=self.root/'accounts', daily_authority=True)
        with patch.object(LabRunner, 'probe', return_value=(503, b'')) as probe:
            with self.assertRaisesRegex(RuntimeError, '尚未完成当前数据库迁移检查'):
                LabRunner.start(options)
            probe.assert_called_once_with(18090, '/health/version', limit=8192)
            probe.reset_mock()
            options.daily_authority = False
            with self.assertRaisesRegex(ValueError, 'distinct unprivileged lab ports'):
                LabRunner.start(options)
            options.daily_authority = True
            options.study_port = 18090
            with self.assertRaisesRegex(ValueError, 'distinct unprivileged lab ports'):
                LabRunner.start(options)
            probe.assert_not_called()

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

    def test_assistant_pages_apis_and_client_boundaries_have_explicit_first_matches(self):
        log = self.root / "access.jsonl"
        cert = self.root / "approved.crt"
        cert.touch()  # The live Envoy test validates certificate bytes.
        base = render(18095, 19097, 18090, log, owns_upstream=False, https_port=18445, cert=cert,
                      key=self.root / "approved.key", upstream_tls_port=18443, upstream_tls_ca=cert,
                      upstream_tls_name="192.168.239.135")
        config = add_workspace(base, 18772, ["http://127.0.0.1:18095", "https://192.168.239.135:18445"],
                               serve_frontend=True)
        for name, cluster in (("nebula_http", "nebula_application"), ("nebula_https", "nebula_application_tls")):
            for route, expected_name, backend in (
                ("/phase11/assistant/", "assistant_page", "nebula_study"),
                ("/phase11/shared/PersonalAgent/ContextWindow.js", "assistant_assets", "nebula_study"),
                ("/phase11/shared/AgentRuntime/DagExecutor.js", "agent_runtime", "nebula_study"),
                ("/api/study/session", "study", "nebula_study"),
                ("/api/ai/web/models", "ai_web_models", cluster),
                ("/api/ai/web/generations", "ai_web_generations", cluster),
                ("/api/ai/web/generations/item", "ai_web_generations", cluster),
                ("/api/ai/web/generations-shadow", "api", cluster),
                ("/api/auth/login", "auth_login", cluster),
                ("/events", "sse", cluster),
                ("/ws", "ws", cluster),
            ):
                found = selected_route(config, name, route)
                self.assertEqual((found["name"], found["route"]["cluster"]), (expected_name, backend))
                self.assertIn(expected_name, ROUTES)
            redirect = selected_route(config, name, "/phase11/assistant")
            self.assertEqual(redirect["redirect"]["path_redirect"], "/phase11/assistant/")
            self.assertEqual(selected_route(config, name, "/api/local/assistant/generations")["direct_response"]["status"], 404)
        self.assertEqual(selected_route(config, "nebula_http", "/api/ai/private/generations/item")["direct_response"]["status"], 403)
        self.assertEqual(selected_route(config, "nebula_https", "/api/ai/private/generations/item")["route"]["cluster"], "nebula_application_tls")
        secure = next(item for item in config["static_resources"]["clusters"] if item["name"] == "nebula_application_tls")
        validation = secure["transport_socket"]["typed_config"]["common_tls_context"]["validation_context"]
        self.assertEqual(validation["match_typed_subject_alt_names"],
                         [{"san_type": "IP_ADDRESS", "matcher": {"exact": "192.168.239.135"}}])
        self.assertEqual(Path(validation["trusted_ca"]["filename"]), cert)

    def test_private_upstream_is_unavailable_without_verified_tls_configuration(self):
        log = self.root / "access.jsonl"
        cert, key = self.root / "fixture.crt", self.root / "fixture.key"
        config = render(18095, 19097, 38100, log, https_port=18445, cert=cert, key=key)
        self.assertEqual(selected_route(config, "nebula_https", "/api/ai/private/models")["direct_response"]["status"], 503)
        with self.assertRaises(ValueError):
            render(18095, 19097, 38100, log, upstream_tls_port=18446)
        with self.assertRaises(ValueError):
            render(18095, 19097, 38100, log, https_port=18445, cert=cert, key=key, upstream_tls_port=18446)

    def test_startup_refuses_gateway_served_stale_asset_bytes(self):
        with patch.object(LabRunner, 'probe', return_value=(200, b'older-static-file')):
            with self.assertRaisesRegex(RuntimeError, '未读取本次发布的资源'):
                LabRunner.verify_assistant_routes(18095, ROOT / 'frontend/phase11')


if __name__ == "__main__":
    unittest.main()
