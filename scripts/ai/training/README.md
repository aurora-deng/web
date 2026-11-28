# AI 训练脚本

当前只有 LoRA（低秩适配）训练，实现在 [lora](lora/README.md)。`train_qwen_lora.py` 仅为兼容入口，不是第二套训练实现。
输入必须是已有批准基础模型和授权 JSONL；输出是 Adapter 与训练 lineage（来源记录）。训练必须显式传 `--train`。
不要在目录迁移或帮助检查中自动安装依赖、下载模型或开始训练。

2026-11-27 隔离烟测已完成：24 条合成记录、CPU 2 步 LoRA 训练，权重确实变化；同一训练产物经修复后导出并由 C++ 成功生成。仅证明链路可运行，不代表训练充分或质量提升；未激活模型。详见 [优化进度](../../../docs/function3/optimization-progress.md)。
