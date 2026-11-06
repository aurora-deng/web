# 当前状态

- **本地开发**：内容与事件投递独立为两个社区装配模块；接口、通信核心和迁移 024 保持不变。[模块边界](docs/architecture/business/engineering/community-runtime-modules.md)
- **复现已验证**：[Linux 从干净源码构建、启动和验证](docs/ops/validation/reproduce.md)。业务档从空目录完成 Release 与迁移 024，112 项 CTest 通过、可选 Redis 跳过；注册、幂等发布、SSE 与重启恢复通过。Windows 79 项前端测试通过。
- **日常服务**：仍以 [Function3.1 发布说明](docs/ops/validation/function3.1.md)为准，本轮没有部署。
- **剩余门槛**：容器草案、远端业务 runner 和十轮 A/A 校准尚未验收。Phase49 的 ACK P99 历史超标结论保留。

[变更流水](docs/ops/changelog.md) · [文档导航](docs/README.md) · [下一阶段](docs/roadmap/next-steps.md)
