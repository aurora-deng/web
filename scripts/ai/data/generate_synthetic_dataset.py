#!/usr/bin/env python3
"""Generate a deterministic, offline-only Phase 22 training smoke dataset.

This generator deliberately creates synthetic records. It is useful for
checking the training/ONNX pipeline, but it is not a replacement for the
consent-gated dataset exported by the application. Real training data must
still come from approved candidates and the database authorization manifest.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import random
import os
import uuid
from typing import Iterable


DATASET_SCHEMA = "phase22-chat-jsonl-v1"
MANIFEST_SCHEMA = "phase11-synthetic-training-manifest-v1"
CONSENT_VERSION = "pipeline-smoke-only"
GENERATOR_VERSION = "1"
DEFAULT_SEED = 2201
MAX_RECORDS = 10_000
MAX_FIELD_CHARS = 32 * 1024


# These are intentionally small, self-contained examples. They exercise the
# same chat JSONL shape as the C++ exporter without reading project data or
# calling an external teacher/model.
EXAMPLES: tuple[tuple[str, str], ...] = (
    (
        "什么是 C++ 中的 RAII？",
        "RAII 是把资源的生命周期绑定到对象生命周期。对象构造时取得资源，析构时自动释放，因此异常路径也能得到清理。文件、锁和连接通常应交给拥有明确所有权的对象管理。",
    ),
    (
        "unique_ptr 和 shared_ptr 应该如何选择？",
        "默认优先使用 unique_ptr，因为它只有一个明确所有者、开销小且不容易形成循环引用。只有确实存在共享所有权时才使用 shared_ptr，并配合 weak_ptr 打破可能的引用环。",
    ),
    (
        "epoll 为什么常用于高并发服务器？",
        "epoll 让内核集中监听大量文件描述符，并只返回发生事件的描述符。它解决的是 I/O 事件发现问题；真正的高并发还需要非阻塞读写、事件循环、任务调度和资源限制共同配合。",
    ),
    (
        "边沿触发的 epoll 为什么必须循环读到 EAGAIN？",
        "边沿触发只在状态从无事件变为有事件时通知一次。如果没有把接收缓冲区读空，后续状态没有再次跨越边界，可能就不会再次通知。因此必须使用非阻塞 I/O 循环读取，直到返回 EAGAIN。",
    ),
    (
        "TCP send 一次没有发完数据时应该怎么办？",
        "send 的返回值可能小于请求长度，表示内核只接受了部分数据。服务端应保存未发送区间，在可写事件到来时继续发送，并正确处理 EAGAIN、连接关闭和错误，而不能假设一次 send 就完成。",
    ),
    (
        "如何减少 C++ 多线程中的死锁风险？",
        "为锁建立全局加锁顺序，并始终按同一顺序获取多个锁；缩短临界区；优先使用 scoped_lock 等 RAII 工具；必要时使用 try_lock 和超时策略。死锁排查还要记录线程、锁和等待关系。",
    ),
    (
        "CMake 中 target_link_libraries 的作用是什么？",
        "它把目标与库建立链接关系，同时可以传播编译和链接需求。应优先使用 PRIVATE、PUBLIC、INTERFACE 明确依赖可见性，避免把实现细节无意传播给所有下游目标。",
    ),
    (
        "数据库查询为什么要使用参数化语句？",
        "参数化语句把 SQL 结构和外部数据分开，数据库不会把输入内容重新解析成 SQL 语法，从而降低 SQL 注入风险。它还利于复用执行计划和统一处理类型转换。",
    ),
    (
        "HTTP ETag 和 304 是如何配合的？",
        "服务端先用 ETag 标识当前资源版本。客户端再次请求时发送 If-None-Match；如果标签仍相同，服务端返回 304 且不发送资源正文，客户端继续使用本地缓存。",
    ),
    (
        "gRPC 的流式响应和普通响应有什么区别？",
        "普通 RPC 通常一次返回完整消息；流式响应可以在一个 HTTP/2 stream 上分批发送多条消息。客户端应处理逐条到达、取消、截止时间、流结束和部分失败，而不是只等待一个大字符串。",
    ),
    (
        "为什么导出 ONNX 前要校验基础模型哈希？",
        "LoRA 或 Adapter 的参数只能解释为针对特定基础模型训练的增量。如果基础权重、提交版本或文件内容发生变化，适配器可能不兼容，因此导出前应验证 manifest、版本和 SHA-256。",
    ),
    (
        "LoRA 微调为什么能减少可训练参数？",
        "LoRA 冻结基础模型，只在目标线性层旁增加低秩矩阵。训练时更新的是这些小矩阵，推理时再以 Adapter 形式加载，从而减少显存和存储开销，同时保留切换基础模型的能力边界。",
    ),
)


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def build_records(count: int, seed: int) -> list[dict[str, object]]:
    if count < 1 or count > MAX_RECORDS:
        raise ValueError(f"--count must be between 1 and {MAX_RECORDS}")

    order = list(range(len(EXAMPLES)))
    random.Random(seed).shuffle(order)
    records: list[dict[str, object]] = []
    seen: set[tuple[str, str]] = set()
    for index in range(count):
        topic, response = EXAMPLES[order[index % len(order)]]
        cycle = index // len(order)
        prompt = topic if cycle == 0 else f"{topic}\n请从第 {cycle + 1} 个独立例子重新说明。"
        pair = (prompt, response)
        if pair in seen:
            raise RuntimeError("generator produced a duplicate prompt/response pair")
        seen.add(pair)
        if len(prompt) > MAX_FIELD_CHARS or len(response) > MAX_FIELD_CHARS:
            raise ValueError("generated field exceeds the training input limit")
        records.append(
            {
                "schema": DATASET_SCHEMA,
                "candidate_id": index + 1,
                "consent_version": CONSENT_VERSION,
                "messages": [
                    {"role": "user", "content": prompt},
                    {"role": "assistant", "content": response},
                ],
            }
        )
    return records


def render_jsonl(records: Iterable[dict[str, object]]) -> bytes:
    lines = [
        json.dumps(record, ensure_ascii=False, separators=(",", ":"))
        for record in records
    ]
    return ("\n".join(lines) + "\n").encode("utf-8")


def atomic_write(path: Path, payload: bytes, force: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and not force:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    temporary_path: Path | None = None
    for _ in range(16):
        candidate = path.with_name(
            f".{path.name}.tmp-{os.getpid()}-{uuid.uuid4().hex}"
        )
        try:
            with candidate.open("xb") as temporary:
                temporary.write(payload)
            temporary_path = candidate
            break
        except FileExistsError:
            continue
    if temporary_path is None:
        raise RuntimeError(f"could not allocate a temporary file beside {path}")
    try:
        os.replace(temporary_path, path)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="output JSONL path")
    parser.add_argument("--count", type=int, default=24)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument(
        "--manifest",
        type=Path,
        help="manifest path; defaults to <output>.manifest.json",
    )
    parser.add_argument("--force", action="store_true", help="replace existing outputs")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = args.output.expanduser().resolve()
    manifest_path = args.manifest or output.with_name(output.name + ".manifest.json")
    manifest_path = manifest_path.expanduser().resolve()
    if manifest_path == output:
        raise ValueError("manifest path must differ from the dataset path")
    records = build_records(args.count, args.seed)
    payload = render_jsonl(records)
    checksum = sha256_bytes(payload)
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "generator_version": GENERATOR_VERSION,
        "dataset_schema": DATASET_SCHEMA,
        "consent_version": CONSENT_VERSION,
        "synthetic": True,
        "real_training_allowed": False,
        "purpose": "offline training and ONNX pipeline smoke validation",
        "seed": args.seed,
        "record_count": len(records),
        "dataset_file": output.name,
        "dataset_bytes": len(payload),
        "dataset_sha256": checksum,
        "generated_at_utc": utc_now(),
    }
    atomic_write(output, payload, args.force)
    atomic_write(
        manifest_path,
        (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"),
        args.force,
    )
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())