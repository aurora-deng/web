# 小研、网页推理与私人推理 · 业务说明

小研的独立聊天和全局侧栏共享个人偏好。在浏览器应用层编排提取偏好、召回、上下文压缩和回答，角色与页面入口仍按产品场景区分。网页模式调用 `/api/ai/web/generations` 及配置的模型连接；Windows 私人模式先在本机处理资料，仅传送本轮授权的上下文到限时私人推理接口。群聊 AI 使用群内上下文。

共享聊天服务可以持久化群内消息，私人推理接口的状态、日志和存储采用独立边界。浏览器与 Windows 还分别承担对应的个人资料和配置处理。上线条件包括验证实际 Provider、流式取消、长对话截断或摘要回退、清理账号上下文、跨账号隔离和凭据保护。

[网页小研编排](../../../frontend/phase11/shared/PersonalAgent/PersonalAgent.js) · [私人推理实现](../../../backend/ai/private) · [个性化进度](../../reports/xiaoyan-personalization-progress.md) · [AI 架构](../../architecture/business/ai/README.md)

[实操学习](learning.md) · [业务目录](../README.md)
