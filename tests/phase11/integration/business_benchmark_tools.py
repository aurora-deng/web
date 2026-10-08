#!/usr/bin/env python3

"""Phase 15/26 基准编排单元测试，不启动服务、不访问数据库。"""

from __future__ import annotations

import argparse
import importlib.util
import sys
import threading
import types
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


benchmark = load_module(
    "phase15_business_benchmark",
    PROJECT_ROOT / "benchmarks" / "phase15_business_benchmark.py",
)
aggregate = load_module(
    "phase15_aggregate",
    PROJECT_ROOT / "benchmarks" / "phase15_aggregate.py",
)


class FakeApi:
    user_id = 1

    def request(self, _method: str, _path: str, _body=None):
        return 200, []

    def close(self) -> None:
        pass


class FakeWebSocket:
    def __init__(self, _api: FakeApi) -> None:
        self.connected = False

    def connect(self) -> None:
        self.connected = True

    def ping(self, _payload: bytes = b"capacity") -> None:
        if not self.connected:
            raise AssertionError("not connected")

    def close(self) -> None:
        self.connected = False


def arguments(**overrides):
    values = {
        "host": "127.0.0.1",
        "port": 18090,
        "timeout": 1.0,
        "server_pid": 0,
        "workers": 1,
        "operations": 1,
        "websocket_connection_steps": [1, 3],
        "websocket_open_workers": 2,
        "websocket_connections_per_user": 4,
        "websocket_settle_seconds": 0.0,
        "ai_user_id": 42,
        "ai_jobs": 2,
        "ai_concurrency": 2,
        "ai_mixed_jobs": 2,
        "ai_mixed_concurrency": 2,
        "ai_mixed_chat_operations": 1,
        "ai_max_events": 10,
        "ai_prompt": "test",
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class BenchmarkToolsTest(unittest.TestCase):
    def test_connection_steps_are_strictly_increasing(self) -> None:
        self.assertEqual(benchmark.connection_steps("10, 20,50"), [10, 20, 50])
        self.assertEqual(benchmark.connection_steps(""), [])
        for invalid in ("0,2", "2,1", "2,2", "a,2"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(argparse.ArgumentTypeError):
                    benchmark.connection_steps(invalid)

    def test_resident_websocket_scale_is_recorded(self) -> None:
        suite = benchmark.BusinessBenchmark(arguments())
        suite.create_user = lambda label: benchmark.UserFixture(  # type: ignore[method-assign]
            label, "password", FakeApi())
        original = benchmark.WebSocketClient
        benchmark.WebSocketClient = FakeWebSocket
        try:
            suite.benchmark_websocket_residency()
        finally:
            benchmark.WebSocketClient = original
        result = suite.results[0]
        self.assertEqual(result["operations"], 3)
        self.assertEqual(result["errors"], 0)
        self.assertEqual(
            [point["establishedConnections"]
             for point in result["connectionScale"]],
            [1, 3],
        )

    def test_ai_mixed_workload_releases_workers_together(self) -> None:
        suite = benchmark.BusinessBenchmark(arguments())
        user = benchmark.UserFixture("user", "password", FakeApi())
        pair = benchmark.PairFixture(user, user, 7)
        suite.create_pair = lambda _label: pair  # type: ignore[method-assign]
        suite._message_channels = lambda _pairs: [(  # type: ignore[method-assign]
            FakeWebSocket(FakeApi()), FakeWebSocket(FakeApi()))]
        suite._create_ai_fixture = lambda _label: object()  # type: ignore[method-assign]

        def fake_generation(_self, _fixture, _index: int, _label: str,
                            gate: threading.Event | None = None,
                            on_ready=None):
            if on_ready:
                on_ready()
            if gate and not gate.wait(1.0):
                raise TimeoutError("gate")
            return 0.01, 0.02, 3

        suite._ai_generation = types.MethodType(fake_generation, suite)
        suite.benchmark_ai_mixed()
        self.assertEqual([item["name"] for item in suite.results], [
            "mixed-with-ai-chat", "mixed-with-ai-generation"])
        self.assertTrue(all(item["errors"] == 0 for item in suite.results))
        self.assertEqual(suite.results[1]["readyWorkers"], 2)

    def test_aggregate_keeps_ai_and_connection_capacity_fields(self) -> None:
        report = {
            "schema": "phase15-business-benchmark-v1",
            "configuration": {},
            "target": "http://127.0.0.1:18090",
            "elapsedSeconds": 1.0,
            "resources": {"available": False},
            "applicationMetrics": {},
            "results": [{
                "name": "ai-generation",
                "operations": 1,
                "units": 3,
                "elapsedSeconds": 1.0,
                "operationsPerSecond": 1.0,
                "unitsPerSecond": 3.0,
                "latencyMs": {"average": 20.0, "p50": 20.0, "p95": 20.0,
                              "p99": 20.0, "maximum": 20.0},
                "errors": 0,
                "errorSamples": [],
                "concurrency": 1,
                "timeToFirstTokenMs": {"p50": 10.0, "p95": 10.0, "p99": 10.0},
                "tokensPerGenerationAverage": 3.0,
                "connectionScale": [{
                    "requestedConnections": 10,
                    "establishedConnections": 10,
                    "connectionErrors": 0,
                    "estimatedBytesPerConnection": 1024.0,
                }],
            }],
        }
        baseline_chat = dict(report["results"][0])
        baseline_chat.update({
            "name": "mixed-50-list-20-history-30-send",
            "latencyMs": {"average": 10.0, "p50": 10.0, "p95": 10.0,
                          "p99": 10.0, "maximum": 10.0},
        })
        ai_chat = dict(baseline_chat)
        ai_chat.update({
            "name": "mixed-with-ai-chat",
            "latencyMs": {"average": 15.0, "p50": 15.0, "p95": 15.0,
                          "p99": 15.0, "maximum": 15.0},
        })
        report["results"].extend([baseline_chat, ai_chat])
        summary = aggregate.aggregate_reports([report], ["run.json"])
        scenario = summary["scenarios"][0]
        self.assertEqual(scenario["timeToFirstTokenMsMedian"]["p95"], 10.0)
        self.assertEqual(
            scenario["connectionScaleMedian"][0]["establishedConnectionsMedian"],
            10.0,
        )
        self.assertEqual(
            summary["comparisons"]["ordinaryChatDuringAi"]["p99ChangePercent"],
            50.0,
        )


if __name__ == "__main__":
    unittest.main()
