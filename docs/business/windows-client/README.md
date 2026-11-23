# Windows Companion 与私人资料 · 业务说明

Windows Companion 提供本机入口、HTTPS/WSS 上游访问和本地助手 API。浏览器通过 `/api/client-capabilities` 发现站点支持的功能。私人资料、索引、历史、凭据和本地会话按站点、账号分隔。普通站点上下游只接受已配置目标和有效身份。

本地客户端承载私密资料，研途账号同步使用用户主动进入的独立账号工作区。上线或分发新客户端前，以实际二进制核对证书信任、Host/Origin 校验、登出切号、长流恢复和本地数据迁移。Linux ZIP 的适用范围是服务器端。

[Windows 程序入口](../../../client/companion/CompanionMain/CompanionMain.cpp) · [本地助手服务](../../../client/assistant/LocalAssistantService/LocalAssistantService.cpp) · [客户端分层](../../architecture/business/client-ui/README.md)

[实操学习](learning.md) · [业务目录](../README.md)
