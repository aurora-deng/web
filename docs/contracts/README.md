# 接口契约

|文件|作用|
|---|---|
|[business-routes.json](business-routes.json)|从实际C++注册生成的120项HTTP方法/路径和领域归属，含3项健康接口|
|[chat-workspace.openapi.json](chat-workspace.openapi.json)|聊天工作区的字段、分页和错误约定|
|[realtime-contracts.json](realtime-contracts.json)|WebSocket命令和SSE事件约定|

路由目录不等于全量字段Schema，权限仍由服务端执行；Windows本地私人接口与外部模型API有独立边界。[接口教学](../architecture/business/guides/phase49-teaching.md)解释前后端如何联系。

站点语音新增 `PATCH /api/admin/speech/credentials`：HTTPS、Session、CSRF 与站点管理员权限；请求为字符串字段 `provider`（azure/gemini）、`apiKey`（只写）、`removeKey`（true/false）。成功只返回保存状态；不会回显 Key。[设置与播放契约说明](../ops/runtime/speech-settings-and-playback.md)。
