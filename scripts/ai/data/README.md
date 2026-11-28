# Phase 11 数据生成器

`generate_synthetic_dataset.py` 生成**纯合成**的 `phase22-chat-jsonl-v1`
数据集，供训练、Adapter 导出和 ONNX 运行链路做离线冒烟验证。

它不会读取聊天记录、数据库、环境变量或模型，也不会请求 Ollama、外部
teacher 或其他网络服务。输出包括：

- `*.jsonl`：与 `train-qwen-lora.py` 相同的 `messages` 数据结构；
- `*.jsonl.manifest.json`：固定 seed、样本数量、字节数和 SHA-256；
- `consent_version=pipeline-smoke-only`：训练脚本只能以 `--pipeline-smoke`
  模式接收它。

## 使用

```bash
python3 scripts/ai/data/generate_synthetic_dataset.py \
  --output /tmp/phase11-smoke.jsonl \
  --count 24 \
  --seed 2201
```

读取 manifest 中的 `dataset_sha256`，再运行训练脚本的校验模式：

```bash
python3 scripts/phase11/train-qwen-lora.py \
  --train \
  --source /path/to/qwen2.5-0.5b-instruct/source \
  --dataset /tmp/phase11-smoke.jsonl \
  --expected-dataset-sha256 <manifest 中的 SHA-256> \
  --pipeline-smoke \
  --output /tmp/phase11-smoke-adapter \
  --validate-only
```

这不是生产训练授权。真实用户数据必须经过应用内明确同意、脱敏、管理员
离线审核和数据库授权 manifest；其 JSONL 应由 C++ 的
`JsonlTrainingDataExporter` 导出，不能用本生成器替代。