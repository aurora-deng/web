# 账号、个人资料与联系人 · 学习与复验

1. 先把会话理解为一张带有效期的门卡。浏览器持有 Cookie，服务器每次还要核对这张门卡是否有效。使用隔离库创建 A、B、C 三个演示账号，比较匿名和 A 登录后的 `GET /api/me`。
2. 让 A 搜索并申请 B，重复提交，B 接受，再让 C 搜索 B。记下联系人列表与公开资料的区别，观察各次 HTTP 状态码及库内关系条数。
3. 让 B 屏蔽 A，再试申请和打开私聊。随后退出 A 并重试 `GET /api/me`，预期旧会话被拒绝。定位判断发生在业务服务还是仓储查询。

在已构建的业务档执行 `ctest --test-dir "$WEB_BUILD" -R 'phase11_http_auth_tests|function3_workspace_contract' --output-on-failure`。练习记录只写临时账号编号、状态码和关系数量。

[业务档复现](../../ops/validation/reproduce.md) · [数据库分层](../../database/domain-layering.md)

[业务说明](README.md) · [业务目录](../README.md)
