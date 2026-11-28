# 模型注册与评测

现有 ModelRegistryService（含路由）、ModelEvaluationService 与 DeterministicExactScorer。
输入为授权模型版本、绑定与评测记录；输出为受质量门槛约束的激活 / 选择或确定性评分。
来源、校验和、试装、角色权限和回滚语义不随目录迁移改变。评测夹具不等于真实模型质量验收。
