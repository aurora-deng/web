"""Contract tests for instance identity, bounded telemetry and alert policy."""

from dataclasses import replace
import hashlib
from io import BytesIO
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.observability.laboratory import isolated_dual
from tools.observability.laboratory.alert_receiver import project
from tools.observability.pipeline.install_pipeline import PACKAGES, VerifiedStream, extract_programs
from tools.observability.laboratory.lab_config import build
from tools.observability.observer.traces.local_traces import TraceIndex
from tools.observability.observer.analysis.projection import trace_view
from tools.observability.observer.metrics.prometheus_service_levels import EPOCH, report
from tools.observability.observer.sources.sources import HTTP_RESPONSES
from tools.observability.observer.storage.store import Store


def matrix(start, end, instance="nebula-b", fail=None):
    stamps = list(range(start, end+1, 15))

    def series(name, values, **labels):
        return {"metric": {"__name__": name, "job": "nebula", "instance": instance, **labels},
                "values": [[stamp, str(value)] for stamp, value in zip(stamps, values)]}

    increments = [5 * index for index in range(len(stamps))]
    return [series("up", [int(stamp != fail) for stamp in stamps]), series(EPOCH, [start]*len(stamps)),
            *[series(HTTP_RESPONSES, increments if i == 2 else [0]*len(stamps), status_class=f"{i}xx")
              for i in range(1, 6)],
            series("nebula_http_handler_seconds_count", increments, route="conversations"),
            series("nebula_http_handler_seconds_bucket", increments, route="conversations", le="0.08"),
            series("nebula_chat_stage_seconds_count", increments, stage="ack_ready"),
            series("nebula_chat_stage_seconds_bucket", increments, stage="ack_ready", le="0.04")]


class Prom:
    def __init__(self, matrix_rows):
        self.rows = matrix_rows

    def prom(self, _, query):
        if query["query"] == 'up{job="nebula"}':
            return {"result": [{"metric": {"instance": key}} for key in ("nebula-a", "nebula-b")]}
        if 'instance="nebula-b"' not in query["query"]:
            raise ValueError("Expected the explicit replica instance scope")
        return {"resultType": "matrix", "result": self.rows}


class DistributedObservability(unittest.TestCase):
    def test_dual_lab_requires_an_independent_loopback_only_network(self):
        with patch.dict(os.environ, {isolated_dual.PARENT_NETWORK: ""}):
            with self.assertRaisesRegex(RuntimeError, "isolated_dual"):
                isolated_dual.require_isolated_network()
        with patch.dict(os.environ, {isolated_dual.PARENT_NETWORK: "net:[100]"}):
            with patch.object(isolated_dual.os, "readlink", return_value="net:[200]") as namespace:
                with patch.object(isolated_dual.socket, "if_nameindex", return_value=[(1, "lo")]) as devices:
                    self.assertEqual(isolated_dual.require_isolated_network(), {
                        "hostNamespace": "net:[100]", "labNamespace": "net:[200]", "interfaces": ["lo"]})
                    namespace.return_value = "net:[100]"
                    with self.assertRaisesRegex(RuntimeError, "separate"):
                        isolated_dual.require_isolated_network()
                    namespace.return_value = "net:[200]"
                    devices.return_value = [(1, "lo"), (2, "ens33")]
                    with self.assertRaisesRegex(RuntimeError, "loopback-only"):
                        isolated_dual.require_isolated_network()

    def test_dual_lab_stops_before_launch_if_host_listener_is_reachable(self):
        with patch.object(isolated_dual, "require_isolated_network", return_value={}):
            with patch.object(isolated_dual.socket, "socket") as socket_factory:
                socket_factory.return_value.__enter__.return_value.connect_ex.return_value = 0
                with patch.object(isolated_dual.subprocess, "Popen") as launch:
                    with self.assertRaisesRegex(RuntimeError, "host loopback"):
                        isolated_dual.exercise(SimpleNamespace(host_probe_port=18785))
                    launch.assert_not_called()

    def test_generated_policy_uses_window_samples(self):
        ports = {name: 18000+index for index, name in enumerate(("observer", "prometheus", "node", "probe", "probe_b",
            "collector", "collector_metrics", "tempo", "tempo_grpc", "tempo_ingest", "alertmanager", "alert_receiver"))}
        data = build(Path("lab"), ports, [("nebula-a", 20000, "probe"), ("nebula-b", 20001, "probe_b")], full=True)
        self.assertEqual({entry["labels"]["instance"] for entry in data["nebula-targets.json"]}, {"nebula-a", "nebula-b"})
        names = {row["job_name"] for row in data["prometheus.yaml"]["scrape_configs"]}
        self.assertTrue({"collector", "tempo", "alertmanager", "nebula", "probe"} <= names)
        rules = {row["alert"]: row["expr"] for row in data["alerts.yaml"]["groups"][0]["rules"]}
        self.assertNotIn("NebulaDatabaseMaxSlow", rules)
        self.assertIn("histogram_quantile(0.99", rules["NebulaDatabaseSlowP99"])
        self.assertIn(">= 200", rules["NebulaDatabaseSlowP99"])
        self.assertIn(">= 100", rules["NebulaHttpHighErrors"])
        receiver = data["alertmanager.yaml"]["receivers"][0]["webhook_configs"][0]
        self.assertTrue(receiver["send_resolved"])
        self.assertTrue(receiver["url"].startswith("http://127.0.0.1:"))
        tempo = data["tempo.yaml"]
        self.assertEqual(tempo["backend_worker"]["compaction"]["block_retention"], "168h")
        self.assertNotIn("compaction", tempo)
        self.assertEqual(tempo["memberlist"]["advertise_addr"], "127.0.0.1")
        for component in ("distributor", "live_store", "metrics_generator", "backend_worker"):
            self.assertEqual(tempo[component]["ring"]["instance_addr"], "127.0.0.1")
        self.assertEqual(Path(tempo["live_store"]["shutdown_marker_dir"]), Path("lab/tempo/live-store/shutdown-marker"))
        self.assertEqual(Path(tempo["backend_scheduler"]["local_work_path"]), Path("lab/tempo/scheduler"))
        retry = data["collector.yaml"]["exporters"]["otlp_http"]["retry_on_failure"]
        self.assertEqual(retry, {"initial_interval": "500ms", "max_interval": "2s", "max_elapsed_time": "5s"})

    def test_streaming_release_verifies_digest_and_retains_upstream_license(self):
        contents = b"Linux executable fixture"
        license_text = b"upstream license fixture"
        archive = BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as bundle:
            program = tarfile.TarInfo("otelcol")
            program.size = len(contents)
            bundle.addfile(program, BytesIO(contents))
        compressed = archive.getvalue()
        checksum = hashlib.sha256(compressed).hexdigest()
        package = replace(PACKAGES["collector"], checksum=checksum,
                          license_checksum=hashlib.sha256(license_text).hexdigest())
        folder = ROOT / "build-function3/obs-dual-contracts"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as directory:
            root = Path(directory)
            license_file = root / "upstream-license"
            license_file.write_bytes(license_text)
            payload = root / "payload"
            payload.mkdir()
            stream = VerifiedStream(BytesIO(compressed), len(compressed))
            with tarfile.open(fileobj=stream, mode="r|gz") as bundle:
                extract_programs(package, bundle, payload, len(contents)+len(license_text), license_file)
            stream.verify(checksum)
            self.assertEqual((payload / "otelcol").read_bytes(), contents)
            self.assertEqual((payload / "LICENSE").read_bytes(), license_text)
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            VerifiedStream(BytesIO(compressed), len(compressed)).verify("a"*64)
        with self.assertRaisesRegex(RuntimeError, "size budget"):
            VerifiedStream(BytesIO(compressed), len(compressed)-1).verify(checksum)

    def test_slo_uses_only_one_instance_and_disconnect_breaks_coverage(self):
        begin, end = 10000, 10300
        other = matrix(begin, end, instance="nebula-a")
        source = Prom(matrix(begin, end) + other)
        self.assertEqual(report(source, begin, end, "")["status"], "choose_instance")
        reading = report(source, begin, end, "nebula-b")["objectives"][0]["observed"]
        self.assertEqual((reading["state"], reading["events"], reading["bad"]), ("evaluated", 100, 0))
        outage = Prom(matrix(begin, end, fail=end-30))
        interrupted = report(outage, begin, end, "nebula-b")["objectives"][0]["observed"]
        self.assertEqual((interrupted["state"], interrupted["reason"]), ("partial", "scrape_failed"))
        self.assertEqual(interrupted["events"], 5)

    def test_local_trace_preserves_instance_metadata_without_arbitrary_attributes(self):
        folder = ROOT/"build-function3/obs-dual-contracts"
        folder.mkdir(parents=True, exist_ok=True)
        now = time.time_ns()
        attributes = [{"key": key, "value": {"stringValue": value}} for key, value in (
            ("service.name", "nebula-webserver"), ("service.instance.id", "nebula-b"),
            ("deployment.environment.name", "lab"), ("chat.body", "omit-me"))]
        span = {"traceId": "b"*32, "spanId": "e"*16, "name": "GET /api/conversations",
                "startTimeUnixNano": str(now-100), "endTimeUnixNano": str(now)}
        with tempfile.TemporaryDirectory(dir=folder) as directory:
            index = TraceIndex(Store(Path(directory)/"observer.sqlite"))
            index.append({"resourceSpans": [{"resource": {"attributes": attributes}, "scopeSpans": [{"spans": [span]}]}]})
            row = trace_view(index.raw(span["traceId"]))[0]
        self.assertIn("nebula-b", row["place"])
        self.assertEqual(row["resourceAttributes"]["deployment.environment.name"], "lab")
        self.assertNotIn("chat.body", row["resourceAttributes"])

    def test_receiver_keeps_resolution_and_omits_free_form_fields(self):
        notification = {"status": "resolved", "alerts": [{"status": "resolved",
            "labels": {"alertname": "NebulaUnreachable", "instance": "nebula-a", "secret": "remove"},
            "annotations": {"couldContainPrivateText": "remove"}}]}
        self.assertEqual(project(notification)["alerts"], [{"status": "resolved",
            "labels": {"alertname": "NebulaUnreachable", "instance": "nebula-a"}}])
        with self.assertRaises(ValueError):
            project({"status": "firing", "alerts": notification["alerts"]*17})


if __name__ == "__main__":
    unittest.main()
