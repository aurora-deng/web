# 知识库、RAG、模型评测与训练 · 业务说明

知识库入口 `/api/knowledge/documents` 管理按用户授权的资料。RAG 从许可范围召回并附引用。模型注册、训练候选数据、数据集版本、转换、评测及发布回滚是受管理的独立流程。公开树内容是否允许进入候选训练集取决于对应的授权状态。

发布检查记录数据集与模型摘要、评估集、结果、操作者和回滚目标。用于证明接口可调用的合成训练，与用于判断模型质量的真实评测证据分别记录。Function3.6 的车队 ONNX 对照仍保留规则默认配置。

[训练服务](../../../backend/ai/training/TrainingDataService/TrainingDataService.cpp) · [树内容授权](../../../backend/application/modules/tree/application/TreeTrainingService/TreeTrainingService.cpp) · [模型与 RAG 索引](../../architecture/business/ai/README.md) · [M00–M07 评测](../../ops/validation/m00-m07.md)

[实操学习](learning.md) · [业务目录](../README.md)
