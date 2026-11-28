# 账号工作区装配边界

本模块已有明确职责文件，本轮只补导航，不为减少同目录文件数继续拆层，也不调整已有路由、账号权限或持久化格式。

| 文件 | 职责 |
| --- | --- |
| [AccountWorkspace.py](AccountWorkspace.py) | 账号会话恢复、工作区服务装配与 HTTP 路由，按已验证账号选择独立存储 |
| [AgentControl.py](AgentControl.py) | 私有 Agent 控制路由，参数校验、工具/技能调用与审计；维持 read、proposal、external 副作用边界 |
| [UploadParts.py](UploadParts.py) | 有界分片上传与重试处理，使用当前账号的文件存储；会清理过期临时分片，不能当纯只读诊断执行 |
| [FrontendSnapshot.py](FrontendSnapshot.py) | 从指定目录读取前端快照，限制资源类型与大小，不创建第二套业务权限 |
| [GatewayConfig.py](GatewayConfig.py) | 为现有 Envoy 生成独立工作区路由，校验端口与来源，不自动替换正式网关配置 |
| [LabRunner.py](LabRunner.py) | 使用隔离回环端口装配网关和工作区进程；有进程生命周期副作用，必须确认目标与批准 |
| [ReleaseBundle.py](ReleaseBundle.py) | 生成/验证仅含代码的 Linux 运行包；打包不等于批准上传或部署，不打包账号数据与凭据 |

## 请求与数据边界

```text
浏览器 / Windows 客户端
  → Envoy /api/study/ 或私有控制路由
  → AccountWorkspace：向固定 NEBULA 上游确认会话
  → AccountServices：选择该账号的 WorkspaceServices
  → AgentControl / UploadParts / 现有工作区业务
  → 该账号自己的数据目录
```

账号、CSRF 与允许来源检查沿用当前实现，不能用本导航推导出“网关转发就已经可信”。工作区装配属于业务边界，不移动到冻结的通信核心。日常运行数据、上传资料和认证配置不参与目录迁移、Git 提交或运行包。

## 验证与扩展

- [工作台 HTTP](../../../tests/project/test_workbench_http.py)、[Agent 控制集成](../../../tests/project/test_agent_control_integration.py)、[控制路由](../../../tests/project/test_agent_control_routes.py)：已有路由与账号隔离契约。
- [Agent 前端](../../../tests/project/test_agent_control_frontend.cjs)、[权限前端](../../../tests/project/test_personnel_frontend.cjs)：已有前端契约，不替代真实浏览器验收。
- 增加业务工作流时，在业务层组装具体 DAG 定义，复用 [工作流接口](../../workflow/README.md)；单个执行器仍保持有界运行，不把数据库连接或事务能力暴露为 Agent 工具。
- 新的运维命令应放入相应 [脚本职责目录](../../../scripts/README.md)，不要往账号路由文件里追加部署、数据库迁移或进程管理逻辑。

[项目目录边界](../../../docs/architecture/overview/project-layout.md) · [研途运行手册](../../../docs/ops/runtime/nebula-yantu-community.md)
