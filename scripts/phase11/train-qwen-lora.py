#!/usr/bin/env python3
"""Train a small, real Qwen LoRA from a Phase 22 consented JSONL dataset.

The script is deliberately offline: both model and dataset must already exist,
and the caller must provide the database-recorded dataset SHA-256.  It uses a
small manual PyTorch loop so the approved Phase 11 environment does not need the
optional ``datasets`` package.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import random
import shutil
import tempfile
import time
from typing import Any

import torch
from peft import LoraConfig, TaskType, get_peft_model
from torch.optim import AdamW
from torch.utils.data import DataLoader, Dataset
from transformers import AutoModelForCausalLM, AutoTokenizer


EXPECTED_REPOSITORY = "Qwen/Qwen2.5-0.5B-Instruct"
EXPECTED_REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
EXPECTED_MODEL_SHA256 = "fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe"
DATASET_SCHEMA = "phase22-chat-jsonl-v1"
TRAINING_SCHEMA = "phase22-real-lora-training-v1"
TARGET_MODULES = ["q_proj", "k_proj", "v_proj", "o_proj"]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def git_blob_sha1(path: Path) -> str:
    digest = hashlib.sha1()
    digest.update(f"blob {path.stat().st_size}\0".encode("ascii"))
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_source(source: Path) -> dict[str, Any]:
    manifest_path = source / "DOWNLOAD_MANIFEST.json"
    if not manifest_path.is_file():
        raise ValueError("base model DOWNLOAD_MANIFEST.json is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("repository") != EXPECTED_REPOSITORY:
        raise ValueError("unexpected base-model repository")
    if manifest.get("revision") != EXPECTED_REVISION:
        raise ValueError("unexpected base-model revision")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("base-model manifest has no file inventory")
    total_bytes = 0
    model_checksum_seen = False
    for entry in files:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("base-model manifest contains an unsafe path")
        path = source / relative
        if not path.is_file() or path.stat().st_size != entry["size"]:
            raise ValueError(f"base-model file is missing or has the wrong size: {path.name}")
        kind = entry.get("digest_kind")
        actual = (
            sha256(path)
            if kind in {"sha256", "lfs-sha256"}
            else git_blob_sha1(path)
        )
        if kind not in {"sha256", "lfs-sha256", "git-blob-sha1"} or actual != entry.get("digest"):
            raise ValueError(f"base-model checksum mismatch: {path.name}")
        if relative.as_posix() == "model.safetensors":
            if actual != EXPECTED_MODEL_SHA256:
                raise ValueError("base-model weights do not match the approved checksum")
            model_checksum_seen = True
        total_bytes += path.stat().st_size
    if not model_checksum_seen:
        raise ValueError("base-model weights are absent from the manifest")
    if total_bytes != manifest.get("total_bytes"):
        raise ValueError("base-model total byte count does not match its manifest")
    return manifest


def read_dataset(path: Path, expected_sha256: str) -> list[dict[str, str]]:
    if len(expected_sha256) != 64 or any(
        character not in "0123456789abcdef" for character in expected_sha256
    ):
        raise ValueError("--expected-dataset-sha256 must be 64 lowercase hex digits")
    actual_sha256 = sha256(path)
    if actual_sha256 != expected_sha256:
        raise ValueError(
            f"dataset checksum mismatch: expected {expected_sha256}, got {actual_sha256}"
        )

    records: list[dict[str, str]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"invalid JSON on dataset line {line_number}") from error
            messages = record.get("messages")
            if record.get("schema") != DATASET_SCHEMA or not isinstance(messages, list):
                raise ValueError(f"invalid Phase 22 record on line {line_number}")
            if len(messages) != 2 or messages[0].get("role") != "user" or messages[1].get("role") != "assistant":
                raise ValueError(f"dataset line {line_number} must contain one user/assistant pair")
            prompt = messages[0].get("content")
            response = messages[1].get("content")
            if not isinstance(prompt, str) or not prompt.strip():
                raise ValueError(f"empty prompt on dataset line {line_number}")
            if not isinstance(response, str) or not response.strip():
                raise ValueError(f"empty response on dataset line {line_number}")
            consent_version = record.get("consent_version")
            if not isinstance(consent_version, str) or not consent_version:
                raise ValueError(f"missing consent version on dataset line {line_number}")
            records.append({
                "prompt": prompt,
                "response": response,
                "consent_version": consent_version,
            })
    if not records:
        raise ValueError("training dataset is empty")
    return records


class ChatPairs(Dataset[dict[str, list[int]]]):
    def __init__(self, records: list[dict[str, str]], tokenizer: Any, max_length: int):
        self.examples: list[dict[str, list[int]]] = []
        for record in records:
            user = {"role": "user", "content": record["prompt"]}
            assistant = {"role": "assistant", "content": record["response"]}
            prompt_encoded = tokenizer.apply_chat_template(
                [user], tokenize=True, add_generation_prompt=True
            )
            full_encoded = tokenizer.apply_chat_template(
                [user, assistant], tokenize=True, add_generation_prompt=False
            )
            # Transformers 4 returned a list here, while Transformers 5 returns
            # BatchEncoding.  Normalize both without pinning the learning script
            # to one incidental container type.
            prompt_ids = (
                prompt_encoded["input_ids"]
                if "input_ids" in prompt_encoded
                else prompt_encoded
            )
            full_ids = (
                full_encoded["input_ids"]
                if "input_ids" in full_encoded
                else full_encoded
            )
            common = 0
            for prompt_token, full_token in zip(prompt_ids, full_ids):
                if prompt_token != full_token:
                    break
                common += 1
            input_ids = list(full_ids[:max_length])
            labels = [-100] * min(common, len(input_ids)) + input_ids[common:]
            if not input_ids or all(label == -100 for label in labels):
                raise ValueError(
                    "max length removed the assistant answer; increase --max-length"
                )
            self.examples.append({"input_ids": input_ids, "labels": labels})

    def __len__(self) -> int:
        return len(self.examples)

    def __getitem__(self, index: int) -> dict[str, list[int]]:
        return self.examples[index]


def collator(pad_token_id: int):
    def collate(examples: list[dict[str, list[int]]]) -> dict[str, torch.Tensor]:
        width = max(len(example["input_ids"]) for example in examples)
        inputs, labels, masks = [], [], []
        for example in examples:
            padding = width - len(example["input_ids"])
            inputs.append(example["input_ids"] + [pad_token_id] * padding)
            labels.append(example["labels"] + [-100] * padding)
            masks.append([1] * len(example["input_ids"]) + [0] * padding)
        return {
            "input_ids": torch.tensor(inputs, dtype=torch.long),
            "labels": torch.tensor(labels, dtype=torch.long),
            "attention_mask": torch.tensor(masks, dtype=torch.long),
        }

    return collate


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", action="store_true", help="confirm a real training run")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--expected-dataset-sha256", required=True)
    authorization = parser.add_mutually_exclusive_group(required=True)
    authorization.add_argument("--authorization-manifest", type=Path)
    authorization.add_argument(
        "--pipeline-smoke",
        action="store_true",
        help="allow only synthetic records whose consent version is pipeline-smoke-only",
    )
    parser.add_argument("--authorization-max-age-seconds", type=int, default=600)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-steps", type=int, default=20)
    parser.add_argument("--max-length", type=int, default=256)
    parser.add_argument("--learning-rate", type=float, default=2e-4)
    # 现有 Olive Adapter 输入图按 r=4 构建。改变 rank 等于更换插头尺寸，必须
    # 重新生成基础 ONNX 图，不能只转换权重文件。
    parser.add_argument("--rank", type=int, default=4)
    parser.add_argument("--alpha", type=int, default=8)
    parser.add_argument("--seed", type=int, default=2201)
    parser.add_argument("--cpu-threads", type=int, default=4)
    parser.add_argument("--gradient-checkpointing", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if not args.train:
        raise SystemExit("--train is required to acknowledge a real training run")
    if args.max_steps <= 0 or args.max_length < 64:
        raise ValueError("max steps must be positive and max length must be at least 64")
    if args.learning_rate <= 0 or args.rank <= 0 or args.alpha <= 0:
        raise ValueError("learning rate, rank, and alpha must be positive")
    if args.cpu_threads <= 0:
        raise ValueError("CPU thread count must be positive")

    source = args.source.expanduser().resolve(strict=True)
    dataset_path = args.dataset.expanduser().resolve(strict=True)
    output = args.output.expanduser().resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite training output: {output}")
    source_manifest = validate_source(source)
    records = read_dataset(dataset_path, args.expected_dataset_sha256)
    authorization_dataset_id = None
    authorization_mode = "pipeline-smoke"
    if args.pipeline_smoke:
        if any(record["consent_version"] != "pipeline-smoke-only" for record in records):
            raise ValueError("--pipeline-smoke accepts only synthetic smoke records")
    else:
        authorization_path = args.authorization_manifest.expanduser().resolve(strict=True)
        authorization = json.loads(authorization_path.read_text(encoding="utf-8"))
        if authorization.get("schema") != "phase22-training-authorization-v1":
            raise ValueError("invalid training authorization manifest")
        if authorization.get("dataset_sha256") != args.expected_dataset_sha256:
            raise ValueError("authorization manifest references a different dataset")
        authorized_dataset = Path(authorization.get("dataset_file", "")).expanduser().resolve(strict=True)
        if authorized_dataset != dataset_path:
            raise ValueError("authorization manifest references a different dataset path")
        if authorization.get("record_count") != len(records):
            raise ValueError("authorization manifest record count differs from the dataset")
        authorized_at = authorization.get("authorized_at_unix")
        if not isinstance(authorized_at, int) or args.authorization_max_age_seconds <= 0:
            raise ValueError("authorization manifest timestamp is invalid")
        age = int(time.time()) - authorized_at
        if age < -60 or age > args.authorization_max_age_seconds:
            raise ValueError("training authorization manifest is stale")
        authorization_dataset_id = authorization.get("dataset_id")
        if not isinstance(authorization_dataset_id, int) or authorization_dataset_id <= 0:
            raise ValueError("authorization manifest dataset ID is invalid")
        authorization_mode = "database-verified"
    if args.validate_only:
        print(json.dumps({
            "schema": TRAINING_SCHEMA,
            "validated": True,
            "records": len(records),
            "dataset_sha256": args.expected_dataset_sha256,
            "base_revision": source_manifest["revision"],
            "authorization_mode": authorization_mode,
            "dataset_id": authorization_dataset_id,
        }, ensure_ascii=False, indent=2))
        return 0

    torch.set_num_threads(args.cpu_threads)
    random.seed(args.seed)
    torch.manual_seed(args.seed)
    started_at = utc_now()

    tokenizer = AutoTokenizer.from_pretrained(source, local_files_only=True)
    if tokenizer.pad_token_id is None:
        tokenizer.pad_token = tokenizer.eos_token
    data = ChatPairs(records, tokenizer, args.max_length)
    generator = torch.Generator().manual_seed(args.seed)
    loader = DataLoader(
        data,
        batch_size=1,
        shuffle=True,
        generator=generator,
        collate_fn=collator(tokenizer.pad_token_id),
    )

    model = AutoModelForCausalLM.from_pretrained(
        source,
        local_files_only=True,
        dtype=torch.float32,
        low_cpu_mem_usage=True,
    )
    model.config.use_cache = False
    model = get_peft_model(model, LoraConfig(
        task_type=TaskType.CAUSAL_LM,
        inference_mode=False,
        r=args.rank,
        lora_alpha=args.alpha,
        lora_dropout=0.05,
        bias="none",
        target_modules=TARGET_MODULES,
    ))
    if args.gradient_checkpointing:
        model.enable_input_require_grads()
        model.gradient_checkpointing_enable()
    model.train()

    trainable = {
        name: parameter
        for name, parameter in model.named_parameters()
        if parameter.requires_grad
    }
    if not trainable:
        raise RuntimeError("PEFT created no trainable parameters")
    initial = {name: parameter.detach().cpu().clone() for name, parameter in trainable.items()}
    optimizer = AdamW(trainable.values(), lr=args.learning_rate)

    losses: list[float] = []
    iterator = iter(loader)
    for _ in range(args.max_steps):
        try:
            batch = next(iterator)
        except StopIteration:
            iterator = iter(loader)
            batch = next(iterator)
        optimizer.zero_grad(set_to_none=True)
        loss = model(**batch).loss
        if not torch.isfinite(loss):
            raise RuntimeError("training produced a non-finite loss")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(list(trainable.values()), 1.0)
        optimizer.step()
        losses.append(float(loss.detach()))

    delta_square = 0.0
    for name, parameter in trainable.items():
        delta = parameter.detach().cpu() - initial[name]
        delta_square += float(torch.sum(delta * delta))
    delta_l2 = math.sqrt(delta_square)
    if not math.isfinite(delta_l2) or delta_l2 <= 0:
        raise RuntimeError("LoRA parameters did not change")

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-", dir=output.parent))
    try:
        model.save_pretrained(staging, safe_serialization=True)
        tokenizer.save_pretrained(staging)
        weights = staging / "adapter_model.safetensors"
        if not weights.is_file():
            raise RuntimeError("PEFT did not save adapter_model.safetensors")
        manifest = {
            "schema": TRAINING_SCHEMA,
            "trained": True,
            "purpose": "consent-gated supervised LoRA fine-tuning",
            "dataset_sha256": args.expected_dataset_sha256,
            "dataset_records": len(records),
            "authorization_mode": authorization_mode,
            "dataset_id": authorization_dataset_id,
            "base_repository": EXPECTED_REPOSITORY,
            "base_revision": EXPECTED_REVISION,
            "base_weights_sha256": EXPECTED_MODEL_SHA256,
            "seed": args.seed,
            "max_steps": args.max_steps,
            "max_length": args.max_length,
            "learning_rate": args.learning_rate,
            "rank": args.rank,
            "alpha": args.alpha,
            "target_modules": TARGET_MODULES,
            "trainable_parameters": sum(parameter.numel() for parameter in trainable.values()),
            "parameter_delta_l2": delta_l2,
            "losses": losses,
            "weights_sha256": sha256(weights),
            "started_at": started_at,
            "completed_at": utc_now(),
            "torch_version": torch.__version__,
        }
        (staging / "PHASE22_TRAINING_MANIFEST.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        staging.rename(output)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise

    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
