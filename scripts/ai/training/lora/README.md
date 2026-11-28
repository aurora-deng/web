# LoRA 训练与管线夹具

| 实现 | 用途 | 输入 → 输出 |
| --- | --- | --- |
| `train_qwen_lora.py` | 真实训练 | 批准的本地 Qwen 和授权数据 → PEFT Adapter、训练 lineage |
| `create_learning_lora_adapter.py` | 确定性未训练夹具 | 本地基础模型 → 非零测试 Adapter，不能宣称学到了能力 |
| `create_zero_lora_reference.py` | 零权重比较基准 | 已有夹具 → 同形状零权重 Adapter，不是训练产物 |

历史 `scripts/phase11/` CLI 和 `scripts/ai/training/train_qwen_lora.py` 保留兼容转发。
真实训练入口为 `python scripts/phase11/train-qwen-lora.py`，仅显式 `--train` 才执行。查看帮助也可能导入已批准的依赖，缺失时不要自动安装。
依赖沿用既有 torch 2.10.0+cpu、transformers 5.3.0、peft 0.21.2；这不是新下载授权。
本次目录验证只比对字节、CLI 转发和合成文件，不代表真实训练成功或回答质量达标。
