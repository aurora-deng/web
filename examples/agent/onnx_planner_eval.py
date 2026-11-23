"""Evaluate an already installed Qwen2.5 ONNX model and a native proposal gate on an isolated fleet."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
from statistics import median
import sys
from tempfile import TemporaryDirectory
from time import perf_counter_ns


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests/cpp_systems"))

from fleet_process import Lab
from tools.agent.simulation_models import percentile


SYSTEM = (
    "Classify a simulated fleet request using ONLY the facts given. Reply with exactly A, B, or C. "
    "A = intent is review AND at least one task is failed, uncertain, or needs reconciliation. "
    "B = intent is propose AND exactly one task is queued and does not need reconciliation. "
    "C = all other requests, including requests about completed tasks without relevant failures. "
    "Do not output prose or any action."
)


def predict(runtime, model, tokenizer, intent, tasks):
    fields = ("taskId", "phase", "needsReconciliation", "outcome")
    body = json.dumps({"intent": intent, "tasks": [{key: row[key] for key in fields} for row in tasks]},
                      separators=(",", ":"))
    template = f"<|im_start|>system\n{SYSTEM}<|im_end|>\n<|im_start|>user\n{body}<|im_end|>\n<|im_start|>assistant\n"
    start = perf_counter_ns()
    tokens = tokenizer.encode(template)
    params = runtime.GeneratorParams(model)
    params.set_search_options(do_sample=False, max_length=len(tokens) + 6)
    generator = runtime.Generator(model, params)
    generator.append_tokens(tokens)
    while not generator.is_done():
        generator.generate_next_token()
    response = generator.get_sequence(0)[len(tokens):]
    label = tokenizer.decode(response).split("<|im_end|>", 1)[0].strip()
    return (label if label in ("A", "B", "C") else "invalid", len(tokens), len(response),
            (perf_counter_ns() - start) / 1_000_000)


def exercise(lab, runtime, model, tokenizer):
    lab.coordinator(agent_sim=True)
    lab.edge("beta", 2, events=False)
    lab.call("submit", "observed-success", "beta")
    lab.phase("observed-success", "succeeded")
    edge = lab.edge("alpha", 1, 5000, events=False)
    lab.call("submit", "observed-interruption", "alpha")
    lab.phase("observed-interruption", "executing")
    lab.stop(edge)
    lab.edge("alpha", 1, events=False)
    lab.phase("observed-interruption", "uncertain")

    durations = []
    results = []
    for index in range(8):
        queue = f"observed-queue-{index}"
        lab.call("submit", queue, "alpha")
        tests = [("propose", [queue], "B"),
                 ("review", ["observed-interruption", queue, "observed-success"], "A")]
        tests.append(("review", [queue, "observed-success"], "C") if index % 2 == 0 else
                     ("propose", ["observed-interruption"], "C"))
        for intent, ids, expected in tests:
            rows = [lab.call("status", task) for task in ids]
            role = {"READ": "READ"}
            rule, _ = lab.agent(intent, *ids, roles=role, code=2 if expected == "C" else 0)
            rule_label = "C" if rule["proposal"] is None else {"review": "A", "propose": "B"}[intent]
            assert rule_label == expected, (intent, ids, "rule changed")
            label, in_tokens, out_tokens, elapsed = predict(runtime, model, tokenizer, intent, rows)
            durations.append(elapsed)
            accepted = False
            if label in ("A", "B"):
                relevant = [row["taskId"] for row in rows if row["phase"] in ("failed", "uncertain") or row["needsReconciliation"]]
                refs = (relevant or ids[:1]) if label == "A" else ids[:1]
                skill = {"A": "fleet.review_failures", "B": "fleet.propose_sim_reschedule"}[label]
                answer, _ = lab.agent(intent, *ids, roles=role, code=0 if label == expected else 2,
                                      flags=("--candidate", skill, "1", ",".join(refs)))
                accepted = answer["proposal"] is not None
            results.append({"case": len(results) + 1, "expected": expected, "modelChoice": label,
                            "nativeGateAccepted": accepted, "inputTokens": in_tokens, "outputTokens": out_tokens})
    return {"cases": results, "ruleCorrect": len(results), "modelCorrect": sum(row["expected"] == row["modelChoice"] for row in results),
            "modelAcceptedProposals": sum(row["nativeGateAccepted"] for row in results),
            "invalidModelFormat": sum(row["modelChoice"] == "invalid" for row in results),
            "modelLatencyMs": {"p50": median(durations), "p95": percentile(durations, 0.95), "p99": percentile(durations, 0.99)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--model", type=Path, required=True)
    args = parser.parse_args()
    import onnxruntime_genai as runtime

    if not (args.model / "genai_config.json").is_file():
        parser.error("choose an existing Qwen2.5 ONNX directory")
    model = runtime.Model(str(args.model))
    tokenizer = runtime.Tokenizer(model)
    extension = ".exe" if sys.platform == "win32" else ""
    for name in ("coordinator", "ctl", "edge"):
        setattr(args, name, args.build / "fleet/transport/grpc" / ("nebula_fleet_" + name + extension))
    args.agent = args.build / "agent" / ("nebula_fleet_agent" + extension)
    with TemporaryDirectory(prefix="nebula-live-planner-") as scratch:
        lab = Lab(args, Path(scratch))
        try:
            report = exercise(lab, runtime, model, tokenizer)
        finally:
            lab.teardown()
    report.update({"provider": "ONNX Runtime GenAI", "providerVersion": runtime.__version__,
                   "modelConfigSha256": sha256((args.model / "genai_config.json").read_bytes()).hexdigest()})
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
