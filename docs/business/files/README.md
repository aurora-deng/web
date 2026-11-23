# 文件、聊天附件与社区媒体 · 业务说明

文件模块提供上传能力查询、文件消息和访问受限的下载。正文保存在文件目录，数据库保存元数据与归属。聊天附件按会话成员关系授权，社区媒体按当前内容范围授权。群成员退出、帖子隐藏和撤回均需在旧 URL 下载时重新判断。

文件用途、大小和内容类型由服务端能力契约约束。上线配置需准备独立文件目录、可恢复的元数据与文件备份、磁盘水位告警，并核对图片、音频及普通附件的错误处理。

[文件服务](../../../backend/application/modules/files/application/FileService/FileService.cpp) · [聊天附件与能力接口](../../contracts/chat-workspace.openapi.json) · [客户端与前端](../../architecture/business/client-ui/README.md)

[实操学习](learning.md) · [业务目录](../README.md)
