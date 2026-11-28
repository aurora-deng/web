# ONNX 导出与只读检查

| 实现 | 输入 → 输出 | 边界 |
| --- | --- | --- |
| `export_trained_lora_onnx.py` | 批准源模型、训练 Adapter → ONNX 图 / Adapter、lineage | 显式 `--export`，校验来源和基础图，清理另行确认 |
| `inspect_onnx_adapter_inputs.py` | ONNX 图 → Adapter 输入报告 | 不加载外部大权重 |
| `compare_onnx_adapters.py` | 同一兼容图和两个 Adapter → 确定性 logits 比较 | 不把未训练夹具当真实质量提升 |
| `model_checksum.py` | 已有普通文件目录 → sha256-tree-v1 摘要 | 拒绝链接，格式与 C++ Provider 保持一致 |

旧路径保留兼容 CLI。摘要示例：`python scripts/ai/export/model_checksum.py <已有模型目录>`。
Olive 0.13 兼容启动器移到 `../toolchain/olive_oga017_compat.py`；不修改 site-packages。
需要既有 Olive 0.13.0、ONNX 1.23.1、ORT GenAI 0.17.0 等批准工具链；缺失依赖时停止。
2026-11-27 已在批准的隔离环境完成真实 LoRA 产物导出与 C++ 推理烟测。首次推理暴露聊天模板遗漏，现已将 `chat_template.jinja` 加入逐字节复制白名单并复测通过；基础 ONNX 图摘要保持一致。
这不是质量提升或正式模型激活的证明。原失败证据、修复与复测边界见 [优化进度](../../../docs/function3/optimization-progress.md)。
