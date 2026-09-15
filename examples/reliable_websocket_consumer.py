#!/usr/bin/env python3
"""WebSocket 可靠消息的接收端幂等窗口。

服务端采用“至少送达一次”时，同一个 server message id 可能以不同 attempt
再次到达。ReliableInbox 保证在当前进程的保留窗口内只执行业务一次，但每次
收到重复消息仍会重新发送 ACK。

该类面向单线程事件循环。若 process() 会被多个工作线程并发调用，最终业务
幂等应由数据库唯一键或事务保证。
"""

from __future__ import annotations

import time
from collections import OrderedDict
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from enum import Enum
from typing import Any


class ConsumeStatus(str, Enum):
    """一次可靠消息消费的可观察结果。"""

    PROCESSED_AND_ACKED = "processed_and_acked"
    DUPLICATE_ACKED = "duplicate_acked"
    PROCESSING_FAILED = "processing_failed"
    ACK_FAILED = "ack_failed"
    INVALID = "invalid"


@dataclass(frozen=True)
class ConsumeResult:
    status: ConsumeStatus
    message_id: str = ""
    duplicate: bool = False
    error: str = ""


def build_ack(message_id: str) -> dict[str, object]:
    """生成与 2.0 应用信封兼容的 ACK。"""

    return {"v": 1, "type": "ack", "replyTo": message_id}


class ReliableInbox:
    """有界、带过期时间的进程内消费去重窗口。"""

    def __init__(
        self,
        *,
        capacity: int = 4096,
        retention_seconds: float = 120.0,
        max_message_id_bytes: int = 256,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        if capacity <= 0:
            raise ValueError("capacity must be positive")
        if retention_seconds <= 0:
            raise ValueError("retention_seconds must be positive")
        if max_message_id_bytes <= 0:
            raise ValueError("max_message_id_bytes must be positive")

        self._capacity = capacity
        self._retention_seconds = retention_seconds
        self._max_message_id_bytes = max_message_id_bytes
        self._clock = clock
        self._processed: OrderedDict[str, float] = OrderedDict()

    def consume(
        self,
        envelope: Mapping[str, Any],
        process: Callable[[Mapping[str, Any]], None],
        acknowledge: Callable[[str], None],
    ) -> ConsumeResult:
        """处理一封可靠 chat 消息，并在业务成功后 ACK。

        顺序刻意固定为：校验 -> 去重 -> 业务处理 -> 记入窗口 -> ACK。
        ACK 失败时不撤销“已处理”记录；服务端重发后只会再次 ACK，不会重做业务。
        """

        message_id = self._validate(envelope)
        if message_id is None:
            return ConsumeResult(ConsumeStatus.INVALID)

        now = self._clock()
        self._expire(now)
        duplicate = message_id in self._processed

        if not duplicate:
            try:
                process(envelope)
            except Exception as exc:  # 业务失败必须保留给服务端重试
                return ConsumeResult(
                    ConsumeStatus.PROCESSING_FAILED,
                    message_id,
                    error=str(exc),
                )
            self._remember(message_id, now)

        try:
            acknowledge(message_id)
        except Exception as exc:
            return ConsumeResult(
                ConsumeStatus.ACK_FAILED,
                message_id,
                duplicate=duplicate,
                error=str(exc),
            )

        return ConsumeResult(
            ConsumeStatus.DUPLICATE_ACKED
            if duplicate
            else ConsumeStatus.PROCESSED_AND_ACKED,
            message_id,
            duplicate=duplicate,
        )

    def contains(self, message_id: str) -> bool:
        """查询当前窗口；查询同时清理已过期条目。"""

        self._expire(self._clock())
        return message_id in self._processed

    def size(self) -> int:
        self._expire(self._clock())
        return len(self._processed)

    def _validate(self, envelope: Mapping[str, Any]) -> str | None:
        if envelope.get("v") != 1 or envelope.get("type") != "chat":
            return None
        if envelope.get("ack") is not True:
            return None

        message_id = envelope.get("id")
        attempt = envelope.get("attempt")
        if not isinstance(message_id, str) or not message_id:
            return None
        if len(message_id.encode("utf-8")) > self._max_message_id_bytes:
            return None
        if (
            not isinstance(attempt, int)
            or isinstance(attempt, bool)
            or attempt < 1
        ):
            return None
        return message_id

    def _remember(self, message_id: str, now: float) -> None:
        while len(self._processed) >= self._capacity:
            self._processed.popitem(last=False)
        self._processed[message_id] = now

    def _expire(self, now: float) -> None:
        cutoff = now - self._retention_seconds
        while self._processed:
            _, processed_at = next(iter(self._processed.items()))
            if processed_at > cutoff:
                break
            self._processed.popitem(last=False)
