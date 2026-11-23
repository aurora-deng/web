# 账号、个人资料与联系人 · 业务说明

账号模块签发和撤销会话。资料模块决定哪些字段进入对外展示。联系人模块维护好友关系、申请和屏蔽状态。三个模块使用同一个用户标识，通过业务接口完成权限判断。

| 场景 | 路径与权威数据 |
|---|---|
| 登录和退出 | `POST /api/auth/login`、`POST /api/auth/logout` → 账号服务 → PostgreSQL 账号与会话 |
| 个人资料 | `GET /api/me`、`PATCH /api/me/profile-display` → 本人资料和可公开字段的独立投影 |
| 联系人 | `GET /api/users/search` → 最小公开资料 → 好友或屏蔽操作按当前关系校验 |

Cookie 会话确定请求身份。修改资料、关系及会话状态由服务端校验。前端隐藏按钮可以减少误操作。生产入口需核对 HTTPS、退出后会话吊销、好友接受、重复申请和屏蔽后的搜索与发消息行为。

[账号域实现](../../../backend/application/modules/accounts) · [资料域](../../../backend/application/modules/profiles) · [联系人域](../../../backend/application/modules/contacts) · [接口教学](../../architecture/business/guides/phase43-47-teaching.md)

[实操学习](learning.md) · [业务目录](../README.md)
