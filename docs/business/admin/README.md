# 站点管理、权限与审计 · 业务说明

管理接口集中在 `/api/admin/*`。后台根据身份能力显示入口。服务端区分普通用户、内容管理员与站点管理员，分别检查用户状态、举报处理、角色授权、全站声音和模型管理。模型发布、回滚与模型评测仍经过 AI 模块的审批门槛。

站点管理操作应能核对操作者、目标、原因和结果。全站管理员与群管理员属于不同权限范围。上线前用真实数据库验证越权拒绝、最后管理员保护、会话撤销生效及失败操作对审计的影响。

[AdminService](../../../backend/application/modules/admin/application/AdminService/AdminService.cpp) · [ModelAdminService](../../../backend/application/modules/admin/application/ModelAdminService/ModelAdminService.cpp) · [社区管理架构](../../architecture/business/community/README.md) · [管理路由契约](../../contracts/business-routes.json)

[实操学习](learning.md) · [业务目录](../README.md)
