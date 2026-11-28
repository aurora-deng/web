# 人员与权限工作台

入口：`/phase11/admin/permissions/`。页面、JS、CSS 由原生服务检查当前登录会话与站点管理员身份；公开前端快照不分发这些文件。

## 权限设计

- 复用 `user / moderator / administrator`，不根据用户名 `root` 自动提权，不增加硬编码密码或隐藏账号。
- 站点管理员可通过既有 `/api/admin/users/:user` 授予管理员、启停账号和吊销会话。每次写入必须带 CSRF 与审计原因；保留最后一位活跃管理员保护。
- Agent 操作/审批仍分别由 `WEB_AGENT_CONTROL_USER_IDS` 和 `WEB_AGENT_CONTROL_APPROVER_IDS` 显式配置应用用户 ID。站点管理员授权不会隐式扩大该白名单；外部副作用依然需要审批。
- 管理员可访问各应用模块，但不因此取得操作系统 root 身份，也不能冒用其他人的私有会话。

## 经授权的离线管理员维护

优先使用现有管理员页面。若数据库所有者已明确批准离线维护，可在服务器本机执行：

```sh
function3-site-admin --operator-grant-administrator EXISTING_USERNAME REASON --confirm
```

连接复用 `WEB_PHASE11_DATABASE`。只接受已存在、活跃的人类账号；不设置密码、不重置其他账号、不移除管理员。操作使用同一角色锁、参数化事务、会话吊销，并记录 `operator_grant_administrator` 审计，原因明确标注离线数据库所有者维护，不冒称为原管理员的登录操作。重复授权已为管理员的账号不再写入或吊销会话。此工具不提供任何 HTTP 提权接口；仅持有数据库维护权限的本机操作员可用。

首次安装的 `--bootstrap` 保持原有限制：已有活跃管理员时拒绝执行。密码只通过正常注册/登录机制输入，不放入源码、参数或部署回执。
