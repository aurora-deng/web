# 站点管理、权限与审计 · 学习与复验

1. 在隔离库准备普通用户、内容管理员、站点管理员。每种角色分别打开管理页并直接读取 `GET /api/admin/audits`，对比可见入口与 HTTP 权限。
2. 用内容管理员处理测试举报。另用站点管理员做一个可撤回的账户禁用实验，重新登录验证结果，再恢复账户并查审计。比较旧会话和新登录的行为。
3. 沿 AdminService 查角色判断、写操作和审计发生的先后。构造一个请求触发“最后管理员保护”并核对角色记录未变。

[权限与数据层教学](../../architecture/business/guides/phase49-teaching.md) · [业务测试档](../../ops/validation/reproduce.md)

[业务说明](README.md) · [业务目录](../README.md)
