"""Explicit offline synthetic ONNX measurement. No DB, activation, downloads or services.

The built-in suite is a NEW versioned fixture, not the historical DB's 24-case suite.
Open answers require separate review; classification accuracy alone never permits activation.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
from model_selection import add_model_arguments, resolve_selection

SCHEMA = "nebula-model-comparison-v1"
SUITE_VERSION = "synthetic-quality-20261127-v1"
CLASSIFICATION = {
    "positive": (
        "I loved this film and would happily watch it again.",
        "The delivery was quick and the product works perfectly.",
        "Your help made a difficult day much better. Thank you!",
        "The team did an excellent job and I am delighted.",
        "This meal was delicious; I really enjoyed it.",
        "The service was friendly and exceeded my expectations.",
        "I am very pleased with the quality of this purchase.",
        "What a wonderful concert! Every song was fantastic.",
    ),
    "negative": (
        "The film was boring and I regret wasting my time.",
        "My order arrived broken and I am deeply disappointed.",
        "The service was rude and the long wait was awful.",
        "This update made the app unusable and I hate it.",
        "The food tasted terrible; I will not return.",
        "The instructions were confusing and extremely frustrating.",
        "I regret buying this unreliable product.",
        "The concert sound was dreadful and ruined the evening.",
    ),
    "neutral": (
        "The meeting starts at nine o'clock tomorrow morning.",
        "The package contains three cables and one adapter.",
        "The document has twelve pages and two tables.",
        "The train travels between the two cities every day.",
        "This product is available in blue and grey.",
        "The library closes at six on weekdays.",
        "The office is located on the second floor.",
        "The application displays the current date on its home page.",
    ),
}
OPEN_CASES = (
    ("chat-1", "chat", "你好，请用一句中文介绍你能怎样帮助学习编程。"),
    ("chat-2", "chat", "今天调试一直失败，我有点沮丧，请简短鼓励我并给一个具体建议。"),
    ("chat-3", "chat", "我准备先学C++再学Linux后端，请给出三个简短的入门步骤。"),
    ("knowledge-1", "knowledge", "请简要说明 TCP 与 UDP 的两个关键区别，不要声称 UDP 一定更快。"),
    ("knowledge-2", "knowledge", "C++ 中 RAII 是什么？请简短解释它怎样避免资源泄漏。"),
    ("knowledge-3", "knowledge", "Python 中列表和元组有什么主要区别？分别举一个适用场景。"),
)


def cases(suite):
    if suite == "long":
        return [{"id": "long-1", "category": "long", "system": "Answer clearly and accurately.",
                 "prompt": "Write a detailed tutorial in English about a C++ TCP server: socket creation, bind, listen, accept, non-blocking reads, partial writes, connection ownership, timeouts and graceful shutdown. Use numbered sections and examples. Aim for at least 1000 words; keep explaining rather than summarizing early.",
                 "maxTokens": "512"}]
    if suite != "quality":
        raise ValueError("unknown built-in suite")
    result = []
    for index in range(8):
        for label, examples in CLASSIFICATION.items():
            result.append({"id": f"sentiment-{label}-{index+1}", "category": "classification",
                           "system": "You are a sentiment classifier. Output exactly one lowercase word: positive, negative, or neutral. No explanation.",
                           "prompt": examples[index], "maxTokens": "16"})
    result += [{"id": name, "category": category, "system": "Answer briefly and plainly.",
                "prompt": prompt, "maxTokens": "128"} for name, category, prompt in OPEN_CASES]
    return result


def fixture(suite):
    return ("\n".join(json.dumps(c, ensure_ascii=False, separators=(",", ":")) for c in cases(suite)) + "\n").encode("utf-8")


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024*1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_environment(source):
    keys = ("PATH", "LD_LIBRARY_PATH", "LANG", "TZ", "LC_ALL", "LC_CTYPE", "LC_MESSAGES")
    return {key: source[key] for key in keys if key in source}


def cpu_affinity(value, available):
    parts = value.split(",")
    if not parts or any(not p.isascii() or not p.isdigit() for p in parts):
        raise ValueError("CPU affinity requires comma-separated CPU IDs")
    cpus = [int(p) for p in parts]
    if len(cpus) != len(set(cpus)) or len(cpus) > 4 or not set(cpus) <= set(available):
        raise ValueError("CPU set must be distinct, allowed and at most four cores")
    return ",".join(map(str, sorted(cpus)))


def validate_rows(rows, suite, repeats):
    expected = cases(suite)
    if not rows or rows[0].get("kind") != "metadata" or rows[0].get("schema") != "nebula-onnx-benchmark-v1" or rows[0].get("prevalidated") is not False:
        raise ValueError("missing cold-process metadata")
    metadata = rows[0]
    if not isinstance(metadata.get("modelChecksum"), str) or not re.fullmatch(r"sha256-tree-v1:[0-9a-f]{64}", metadata["modelChecksum"]):
        raise ValueError("missing model tree checksum")
    if len(rows) != 1 + repeats*len(expected):
        raise ValueError("missing or extra generations")
    for index, row in enumerate(rows[1:]):
        case = expected[index % len(expected)]
        if any(type(row.get(k)) is not type(v) or row.get(k) != v for k, v in {"kind": "generation", "id": case["id"],
                "category": case["category"], "maxTokens": int(case["maxTokens"]),
                "repeat": index//len(expected)+1, "ordinal": index+1,
                "success": True, "cancelled": False, "timedOut": False,
                "outputLimitExceeded": False, "validUsage": True}.items()):
            raise ValueError("failed, misordered or incomplete generation")
        usage = row.get("usage", {})
        for key in ("promptTokens", "outputTokens", "decodeTokens", "decodeMicroseconds",
                    "firstTokenMicroseconds", "modelLoadMicroseconds", "queueWaitMicroseconds"):
            v = usage.get(key)
            if type(v) is not int or not 0 <= v <= (16_777_216 if key == "promptTokens" else 3_600_000_000):
                raise ValueError("invalid tokenizer measurement")
        if not 1 <= usage["outputTokens"] <= int(case["maxTokens"]) or usage["decodeTokens"] != usage["outputTokens"]-1:
            raise ValueError("token IDs inconsistent with request")
        if usage["decodeTokens"] and not usage["decodeMicroseconds"]:
            raise ValueError("decode token IDs missing elapsed time")
        if not usage["decodeTokens"] and usage["decodeMicroseconds"]:
            raise ValueError("zero decode tokens with nonzero duration")
        if usage.get("modelLoadPerformed") is not (index == 0) or (index and usage["modelLoadMicroseconds"]):
            raise ValueError("model load/cache expectation violated")
        if type(row.get("wallMicroseconds")) is not int or row["wallMicroseconds"] <= 0:
            raise ValueError("missing request wall time")
        ttft = row.get("clientFirstTokenMicroseconds")
        if type(ttft) is not int or not 0 <= ttft <= row["wallMicroseconds"]:
            raise ValueError("missing client first-chunk time")
        if not isinstance(row.get("text"), str) or not row["text"] or len(row["text"].encode()) > 65536:
            raise ValueError("missing or oversized synthetic response")
    return rows[1:]


def summarize(rows, suite, repeats):
    generations = validate_rows(rows, suite, repeats)
    classification = [r for r in generations if r["category"] == "classification"]
    # Strict exact label scoring, not fuzzy sentiment guessing or open-answer evaluation.
    correct = sum(r["text"].strip().lower() == r["id"].split("-")[1] for r in classification)
    summary = {"generations": len(generations), "classification": {
        "correct": correct, "total": len(classification),
        "accuracy": correct/len(classification) if classification else None},
        "manualReviewRequired": any(r["category"] in {"chat", "knowledge"} for r in generations),
        "activationAllowed": False, "categories": {}}
    for category in sorted({r["category"] for r in generations}):
        group = [r for r in generations if r["category"] == category]
        rates = [r["usage"]["decodeTokens"]*1e6/r["usage"]["decodeMicroseconds"]
                 for r in group if r["usage"]["decodeTokens"] and r["usage"]["decodeMicroseconds"]]
        summary["categories"][category] = {
            "count": len(group), "medianDecodeTokensPerSecond": statistics.median(rates) if rates else None,
            "medianProviderFirstTokenSeconds": statistics.median(r["usage"]["firstTokenMicroseconds"] for r in group)/1e6,
            "medianWallSeconds": statistics.median(r["wallMicroseconds"] for r in group)/1e6,
            "outputTokenCounts": [r["usage"]["outputTokens"] for r in group],
            "reachedRequestedLimit": sum(r["usage"]["outputTokens"] == r["maxTokens"] for r in group)}
    first = generations[0]
    summary["coldProcess"] = {"modelLoadSeconds": first["usage"]["modelLoadMicroseconds"]/1e6,
                               "clientFirstChunkSeconds": first["clientFirstTokenMicroseconds"]/1e6,
                               "wallSeconds": first["wallMicroseconds"]/1e6,
                               "loadIncludesChecksumVerification": True,
                               "osPageCacheDropped": False}
    summary["warmRequestCount"] = len(generations)-1
    warm = generations[1:]
    summary["warmMedianClientFirstChunkSeconds"] = statistics.median(r["clientFirstTokenMicroseconds"] for r in warm)/1e6 if warm else None
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_model_arguments(parser)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--suite", choices=("quality", "long"), default="quality")
    parser.add_argument("--repeats", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--cpus", default="2,3")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--write-fixture", type=Path, help="offline fixture creation only; never runs a model")
    args = parser.parse_args()
    if args.write_fixture:
        with args.write_fixture.open("xb") as output:
            output.write(fixture(args.suite))
        print(json.dumps({"synthetic": True, "suiteVersion": SUITE_VERSION, "sha256": hashlib.sha256(fixture(args.suite)).hexdigest()}))
        return 0
    if sys.platform != "linux" or not args.binary or not args.output or not 1 <= args.timeout <= 180:
        parser.error("explicit Linux binary/output and timeout 1-180 required; no implicit remote access")
    import resource
    selected = resolve_selection(args)
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("benchmark binary is not executable")
    cpus = cpu_affinity(args.cpus, os.sched_getaffinity(0))
    available = next(int(line.split()[1]) for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemAvailable:"))
    if available < 3_500_000:
        raise RuntimeError("less than 3.5 GB available; refusing isolated measurement")
    # Refuse overwriting evidence, and never write into a model directory.
    output = args.output.resolve()
    if output.is_relative_to(selected.root):
        raise ValueError("benchmark output cannot be inside the model root")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    payload = fixture(args.suite)
    (output/"cases.jsonl").write_bytes(payload)
    command = ["/usr/bin/taskset", "-c", cpus, "/usr/bin/nice", "-n", "5", str(binary),
               str(selected.root), selected.artifact, str(output/"cases.jsonl"), str(args.repeats), str(args.timeout)]
    if selected.adapter:
        command.append(selected.adapter)
    report = {"schema": SCHEMA, "suiteVersion": SUITE_VERSION, "suite": args.suite,
              "synthetic": True, "model": selected.report(), "binarySha256": digest_file(binary),
              "fixtureSha256": hashlib.sha256(payload).hexdigest(), "repeats": args.repeats,
              "cpuAffinity": cpus, "nice": 5, "caseTimeoutSeconds": args.timeout,
              "activationAllowed": False, "passed": False,
              "limits": ["new suite, not historical database evaluation", "open answers require manual review",
                         "cold process, not cold disk; checksum read primes page cache",
                         "model defaults unchanged; sampler configuration may differ between models",
                         "peak RSS and CPU time are whole-process measurements, not per request",
                         "requested 512 tokens is not measured 512 tokens unless output count reaches it"]}
    started = time.monotonic()
    try:
        with (output/"raw.jsonl").open("wb") as stdout, (output/"stderr.log").open("wb") as stderr:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                    env=safe_environment(os.environ),
                                    timeout=(args.timeout+30)*len(cases(args.suite))*args.repeats+120)
        report["exitCode"] = result.returncode
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        report["process"] = {"wallSeconds": time.monotonic()-started, "peakRssKiB": usage.ru_maxrss,
                             "userCpuSeconds": usage.ru_utime, "systemCpuSeconds": usage.ru_stime}
        if result.returncode:
            raise RuntimeError("benchmark process failed; evidence retained")
        if (output/"raw.jsonl").stat().st_size > 8*1024*1024:
            raise ValueError("benchmark output exceeds evidence limit")
        rows = [json.loads(line) for line in (output/"raw.jsonl").read_text(encoding="utf-8").splitlines()]
        report["summary"] = summarize(rows, args.suite, args.repeats)
        report["passed"] = True
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        # Do not echo command arguments, local paths, environment or provider diagnostics.
        report["failureClass"] = type(error).__name__
    finally:
        (output/"report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
