# 检索增强生成（RAG）

现有 RagService、DeterministicKnowledgeIndex 与 KnowledgeRuntime；分别负责检索用例、确定性索引和依赖组装。
输入必须经过会话 / 用户权限及大小约束，输出为有限上下文和可追溯引用；不扩大跨用户知识访问范围。
目录迁移不改变路由、存储或数据权限，也不授权导出用户文档。
