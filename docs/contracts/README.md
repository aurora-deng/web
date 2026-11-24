# 接口契约

|文件|作用|
|---|---|
|[business-routes.json](business-routes.json)|从实际 C++ 注册生成的 124 项 HTTP 方法与路径，按 18 个领域归属|
|[chat-workspace.openapi.json](chat-workspace.openapi.json)|聊天工作区的字段、分页和错误约定|
|[realtime-contracts.json](realtime-contracts.json)|WebSocket命令和SSE事件约定|

路由目录不等于全量字段Schema，权限仍由服务端执行；Windows本地私人接口与外部模型API有独立边界。[接口教学](../architecture/business/guides/phase49-teaching.md)解释前后端如何联系。

研途账号接口由独立工作区服务提供。`GET /api/study/session` 在登录时返回十进制 `accountId`、`scope`、同源 `url` 和 `csrfToken`，并禁止缓存。401 表示需要登录，网关错误保留重试入口。确认工作区后，请求 `/api/study/{scope}/` 或其数据子路径仍会核对当前会话，账号不符返回 403。[工作区契约与启动](../ops/runtime/nebula-yantu-community.md#function33-首次绑定与切号)

`GET /api/study/{scope}/api/assistant-summary?day=YYYY-MM-DD` 返回十进制字符串 `accountId`、请求日期 `day` 及四项非负整数。 `todayPlanned` 包括当天已完成和待办，`todayCompleted` 是其中已完成的数量，`overdue` 统计此前未完成，`nextSevenDays` 统计明日起七天内未完成。无效日期返回 400，未登录返回 401，跨账号返回 403。响应使用 `Cache-Control: no-store`。 [小研调用流程](../business/assistant/skills.md)

站点语音新增 `PATCH /api/admin/speech/credentials`：HTTPS、Session、CSRF 与站点管理员权限；请求为字符串字段 `provider`（azure/gemini）、`apiKey`（只写）、`removeKey`（true/false）。成功只返回保存状态；不会回显 Key。[设置与播放契约说明](../ops/runtime/speech-settings-and-playback.md)。

Windows 小研通过 `GET /api/local/session/info` 获得当前上游站点标识，从 `/api/local/assistant/capabilities` 判断是否支持辅助推理及历史压缩。辅助任务使用 `/api/local/assistant/preference-inference` 创建、轮询及取消，写操作核对本机 CSRF、账号和绑定会话。主回答沿用本机 `/generations`，长会话可附带 `historySummary` 和十进制字符串 `historyThrough`，摘要上限 1,500 字节，游标须属于当前会话，并纳入幂等检查。[数据范围和请求流程](../reports/xiaoyan-personalization-progress.md)
