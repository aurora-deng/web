#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "examples"))

from reliable_websocket_consumer import ConsumeStatus, ReliableInbox  # noqa: E402


def envelope(message_id: str = "ws-1-1", attempt: int = 1) -> dict[str, object]:
    return {
        "v": 1,
        "type": "chat",
        "id": message_id,
        "attempt": attempt,
        "ack": True,
        "content": "hello",
    }


class ManualClock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class ReliableInboxTest(unittest.TestCase):
    def test_duplicate_is_acked_without_processing_twice(self) -> None:
        processed: list[str] = []
        acknowledgements: list[str] = []
        inbox = ReliableInbox()

        first = inbox.consume(
            envelope(attempt=1),
            lambda message: processed.append(str(message["content"])),
            acknowledgements.append,
        )
        duplicate = inbox.consume(
            envelope(attempt=2),
            lambda message: processed.append(str(message["content"])),
            acknowledgements.append,
        )

        self.assertEqual(first.status, ConsumeStatus.PROCESSED_AND_ACKED)
        self.assertEqual(duplicate.status, ConsumeStatus.DUPLICATE_ACKED)
        self.assertEqual(processed, ["hello"])
        self.assertEqual(acknowledgements, ["ws-1-1", "ws-1-1"])

    def test_processing_failure_is_not_remembered_or_acked(self) -> None:
        acknowledgements: list[str] = []
        inbox = ReliableInbox()

        def fail(_message: object) -> None:
            raise RuntimeError("database unavailable")

        failed = inbox.consume(envelope(), fail, acknowledgements.append)
        retried = inbox.consume(envelope(attempt=2), lambda _message: None,
                                acknowledgements.append)

        self.assertEqual(failed.status, ConsumeStatus.PROCESSING_FAILED)
        self.assertEqual(retried.status, ConsumeStatus.PROCESSED_AND_ACKED)
        self.assertEqual(acknowledgements, ["ws-1-1"])

    def test_ack_failure_keeps_processed_record(self) -> None:
        processed: list[str] = []
        acknowledgements: list[str] = []
        inbox = ReliableInbox()

        def disconnect_before_ack(_message_id: str) -> None:
            raise OSError("connection lost")

        first = inbox.consume(
            envelope(),
            lambda message: processed.append(str(message["content"])),
            disconnect_before_ack,
        )
        retry = inbox.consume(
            envelope(attempt=2),
            lambda message: processed.append(str(message["content"])),
            acknowledgements.append,
        )

        self.assertEqual(first.status, ConsumeStatus.ACK_FAILED)
        self.assertEqual(retry.status, ConsumeStatus.DUPLICATE_ACKED)
        self.assertEqual(processed, ["hello"])
        self.assertEqual(acknowledgements, ["ws-1-1"])

    def test_capacity_and_retention_bound_memory(self) -> None:
        clock = ManualClock()
        inbox = ReliableInbox(capacity=2, retention_seconds=10, clock=clock)
        ack = lambda _message_id: None
        process = lambda _message: None

        inbox.consume(envelope("one"), process, ack)
        clock.now = 1
        inbox.consume(envelope("two"), process, ack)
        clock.now = 2
        inbox.consume(envelope("three"), process, ack)

        self.assertFalse(inbox.contains("one"))
        self.assertEqual(inbox.size(), 2)
        clock.now = 12
        self.assertEqual(inbox.size(), 0)

    def test_malformed_envelope_never_reaches_business(self) -> None:
        processed: list[object] = []
        acknowledgements: list[str] = []
        bad = envelope()
        bad["attempt"] = 0

        result = ReliableInbox().consume(
            bad, processed.append, acknowledgements.append)

        self.assertEqual(result.status, ConsumeStatus.INVALID)
        self.assertEqual(processed, [])
        self.assertEqual(acknowledgements, [])


if __name__ == "__main__":
    unittest.main()
