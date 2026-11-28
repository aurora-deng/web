# AI 数据、训练与导出脚本

本目录是现有流程的实现位置，不是自动训练或下载入口。依赖使用现有批准版本；缺少依赖时停止，不自动安装。

| 阶段 | 实现 | 兼容入口 | 输入 / 输出 |
| --- | --- | --- | --- |
| 数据生成 | `data/generate_synthetic_dataset.py` | 无 | 固定种子的合成数据；详见 data 说明 |
| LoRA 训练 | `training/lora/train_qwen_lora.py` | `../phase11/train-qwen-lora.py` | 已批准的本地 Qwen 源模型和授权 JSONL → Adapter、训练 lineage |
| ONNX 导出 | `export/export_trained_lora_onnx.py` | `../phase11/export-trained-lora-onnx.py` | 源模型、训练 Adapter → ONNX 图 / Adapter、导出 lineage |
| 来源契约 | `contracts/qwen_source.py` | 内部模块 | 校验固定仓库 / revision / 权重 SHA、文件白名单及总字节 |

固定基础模型：`Qwen/Qwen2.5-0.5B-Instruct`，revision `7ae557604adf67be50417f59c2c2f167def9a775`。
来源清单和实际文件必须同时通过校验；不允许重复 JSON 字段、重复文件、链接越界、未知文件和 lineage 不一致。
模型下载清单不是下载权限，本目录不代替人工依赖审批。

## 命令边界

```text
python scripts/phase11/train-qwen-lora.py --help
python scripts/ai/export/export_trained_lora_onnx.py --help
python tests/project/test_ai_pipeline_contracts.py
```

训练脚本顶层需要现有 torch / transformers / peft 环境，因此 `--help` 也可能需要这些依赖；本机未安装时不要为查看帮助而安装。
导出实际执行需显式 `--export`，训练实际执行需显式 `--train`。执行前确认输入授权和 CPU、内存、空间预算。
`--cleanup-work` 会删除指定工作目录，必须另行确认，不能默认开启。

2026-11-27 只读核对的 VM 环境版本：torch 2.10.0+cpu、transformers 5.3.0、peft 0.21.2、olive-ai 0.13.0、onnx 1.23.1、onnxruntime-genai 0.17.0。
这些是当次已安装版本，不是本轮新下载的版本，也不是全部接口兼容性的承诺。

Olive 子进程只继承必要的运行时路径、线程和本地语言环境；不继承云令牌、代理或 PYTHONPATH。
禁用隐式 Hub token、遥测并设置离线标记，但这些标记不是操作系统级网络隔离的替代品。

## 已验证与未验证

合成文件与 mocked Olive 的 7 项契约测试通过，包括来源校验、错误 lineage 在导出前失败、兼容入口与支持文件白名单。
本轮没有实际训练、没有真实 ONNX 导出、没有模型质量 / token/s / RSS 对照，也没有切换日常默认模型。
其余 AI 工具已按 export / training/lora / toolchain 整理，旧 CLI 保留兼容；C++ AI 目录迁移仍未执行，不能称为全部目录重构完成。
Teacher 阶段目前不实现；若未来接入外部 teacher，必须明确来源、鉴权、出站范围和数据授权，不能由本说明自动启用。

## 目录迁移验收

- [训练](training/README.md) / [LoRA](training/lora/README.md) / [导出](export/README.md) / [工具链](toolchain/README.md)。
- 显式清单：[source-ai-script-migration.json](../../docs/architecture/source-ai-script-migration.json)。
- 11 项后续迁移 / 兼容登记：除训练脚本仓库根深度调整外，实现保持字节一致；旧约束和 lock 副本保持一致。
- `python tests/project/test_ai_script_layout.py` 使用源码摘要、mock 导入与合成目录检验，没有导入训练依赖或执行下载。
