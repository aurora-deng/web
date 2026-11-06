# 当前状态

- **开发快照**：`Function3.2` 基于 `Function3.1`，包含社区模块拆分、Linux 复现、连接池恢复和 ACK 分段采样。[项目目录](docs/architecture/overview/project-layout.md) · [版本说明](docs/releases/function3.2.md)
- **复现已验证**：通信档与业务档均从空目录完成 Release 构建和启动。业务档使用临时 PostgreSQL 应用 001–024，112 项 CTest 通过，1 项可选 Redis 契约跳过。注册登录、WebSocket、幂等发布、SSE 与重启恢复通过。干净 Git 检出通过 79 项前端测试和 23 项 Python 工程检查。[复现记录](docs/ops/validation/reproduce.md)
- **日常服务**：仍以 [Function3.1 发布说明](docs/ops/validation/function3.1.md)为准，本轮没有部署。
- **隔离定位**：数据库会话被终止和 ACK 后进程被强杀的恢复演练通过。16 工作端压测尾部显著增加，下一步拆查业务执行器排队及事务提交前路径。[测量结果](docs/releases/function3.2.md#性能定位当前证据指向哪里)
- **剩余门槛**：容器草案、远端业务 runner 和十轮 A/A 校准尚未验收。Phase49 的 ACK P99 历史超标结论保留。

[变更流水](docs/ops/changelog.md) · [文档导航](docs/README.md) · [下一阶段](docs/roadmap/next-steps.md)
