# Teacher 与业务推理的边界

此处预留 AI teacher 领域位置，目前**没有实现**，没有网络调用、自动采集、凭据读取或后台服务。

- `models/OllamaModelProvider` 等现有 Provider 处理业务推理，不由本目录自动改为训练样本生成器。
- 训练数据的来源、外发授权、固定 Provider、鉴权引用、预算和输出审查应在独立的离线生成流程中明确。
- 业务账号数据与 teacher 候选样本隔离；训练产物保留 lineage，不能用“请求成功”代替质量评测。
- 不给 Agent 暴露任意 URL / 文件 / 数据库入口，不将事务控制或真实凭据下放到工具层。

详细的未来准入条件见 [脚本侧 Teacher 说明](../../../scripts/ai/teacher/README.md)。
新增 teacher 行为需要另行设计与批准；此 README 只是目录说明，不是执行权限。
