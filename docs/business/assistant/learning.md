# 小研、网页推理与私人推理 · 学习与复验

1. 用临时账号只写一条显式“记住我偏好分点”。从好友窗口切到侧栏再提问，检查两条聊天历史和共享偏好怎样读取。切到第二个账号确认搜不到这条偏好。
2. 把上下文预算理解为一张有限容量的便签。准备一段足够长的合成对话，确认压缩仍保留近期引用和已确认偏好。分别验证正常模型、摘要失败回退和取消。
3. 在开发机运行 `node --test tests/function3/frontend/agent_runtime.test.mjs`，从测试定位提示组装和访问边界。用两个账号的非敏感固定文案做真实模型的连接、流式和取消验收。记录首 Token 与总耗时。

[上一阶段的 Agent 运行时](../../architecture/business/ai/agent-workflow-runtime.md) · [Windows 客户端](../windows-client/README.md)

[业务说明](README.md) · [业务目录](../README.md)
