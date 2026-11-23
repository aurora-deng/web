# 业务上线台账

Function3.6 的正式交付件是可校验的 Linux ZIP、离线 Web/Fleet 镜像和源码 ZIP。以下记录当前可复验的范围与切换目标环境前需要补齐的证据。测试必须记录实际运行的应用版本、测试数据范围和使用的依赖。 [发布与对比](../releases/function3.6.md) · [部署手册](../ops/runtime/function3.6-deployment.md) · [交付件与虚拟机维护](../ops/runtime/function3.6-artifact-maintenance.md)

| 业务 | 已有验收范围 | 上线前在目标环境核对 |
| --- | --- | --- |
| [账号、资料、联系人](identity/README.md) | PostgreSQL 隔离注册登录、联系人和权限契约 | HTTPS、会话撤销、跨角色可见范围 |
| [缘续聊天](chat/README.md) | 真实 WebSocket、持久化 ACK、双实例送达与历史恢复 | 实际容量下的延迟与重连、权限和撤回复测 |
| [文件和媒体](files/README.md) | 业务目录与附件访问测试 | 配额、附件目录与数据库一起恢复、旧链接权限 |
| [缘生广场](community/README.md) | 真实 HTTP 发布和去重、社区隔离验证 | 内容治理、分享及媒体失效、榜单任务与前端回归 |
| [站点管理](admin/README.md) | 角色与处置路径测试，模型治理测试 | 审计留存、最后管理员恢复、实际管理身份查验 |
| [公告、通知与语音](speech/README.md) | 现有文字通知、音色设置与播放链路的隔离测试 | 实际音色、浏览器授权、Provider 连通、失败回退 |
| [小研](assistant/README.md) | 偏好与长上下文编排契约，网页和私人推理边界 | 实际模型、跨端隔离、超时取消、并发及失败回退 |
| [知识库与训练](model-governance/README.md) | 授权与模型门禁，合成训练和固定 ONNX 对照 | 真实授权数据评测、资源预算及模型回滚 |
| [研途](study/README.md) | 独立身份服务与账号同步隔离验证 | 目标网关路由、跨端切号、冲突、附件和备份恢复 |
| [事件投递与缓存](delivery/README.md) | 事务投递、双实例与恢复；Redis 依赖可选 | 在选用的 Broker 和缓存配置下验证重试、积压与降级 |
| [Observer](observer/README.md) | 真实采集、Tempo 回读、离线恢复及 Alertmanager 通知 | 按实际数据源重走告警、恢复和留存；记录抓取错误和缺段 |
| [Envoy](gateway/README.md) | 研途路由、同账号与静态资源路径的隔离验收 | 当前拓扑验证长连接、上游摘除、证书及回退 |
| [Windows 客户端](windows-client/README.md) | 既有客户端与独立工作区测试 | 目标 Windows 客户端包、HTTPS/WSS、切号与本地隐私复验 |
| [后端 DAG](workflow/README.md) | 原生 CTest、检查点恢复和稳定操作键 | 真实动作的对账、重启与版本升级恢复 |
| [车队协调器和边缘](fleet/README.md) | gRPC 软件边缘与历史跨机验收 | 完整 gRPC TSan 仍需修复验证，ACK P99 需达到 50 ms 参考预算；核对实际传输保护 |
| [车队 Agent](fleet-agent/README.md) | 提案、越权拒绝、审批及模拟适配，规则默认 | 真实模型的评估与回退，操作员权限、审批重放和审计 |
| [Skill 与导航适配](skills/README.md) | 能力合同、软件模拟、Nav2 仿真接入 | M08 之后按独立计划复验硬件、停机和实时行为 |

## 核对运行中的 Web 候选

先校验发布件，准备私有签名密钥、迁移和文件目录，再启动隔离 Web 候选。可从 **Function3.6 源码分支**运行带联机选项的检查脚本，对应已解包的发布目录。

```bash
python3 scripts/project/release_preflight.py --root /path/to/extracted-release --business-port 18096
```

检查器请求回环端口，核对存活、就绪、声明的迁移、修订、源码摘要、线上资源清单摘要，以及匿名会话请求应返回的状态。确认 `localBusinessMatched` 为 `true` 后，再运行隔离的注册登录、WebSocket 及本人权限复验。实际服务启用的外部 Provider、网关、工作区与设备仍按上表检查。

2026-11-23 使用 CentOS 上与发布清单摘要一致的 Web 二进制 `1d991f2a8121` 和独立临时 PostgreSQL 启动。迁移 26、资源版本 `4364119c78f60e44`、就绪与匿名拒绝通过；故意换用旧资源摘要时检查失败。另跑通注册登录、WebSocket 升级、发帖同键去重及 Outbox SSE。验证结束后，临时 Web 和 PostgreSQL 均停止。

同日，以独立临时 PostgreSQL 18 和回环 Redis 复跑该发布构建的 CTest，122 项通过，零跳过；覆盖 Redis 命中、TTL、失效与不可用时退化的契约。Windows 源码检查通过，包含 106 项前端测试及 Observer HTTP 合同；研途临时账号同步测试单独通过。通信核心的 152 个冻结文件通过摘要校验。

## 如何记录一次验收

1. 保存 Git 修订、二进制及资源摘要、迁移号、进程或容器角色、UTC 开始结束、测试档和数据范围。
2. 保存实际通过、失败、未运行及跳过的测试。公网或多机部署另记证书、权限和断连恢复。
3. 保存流量和分位数的样本量、错误数以及先前对照。现有规则路线和部署回退条件随记录一起提交。

[最近 M00–M07 测试与限制](../ops/validation/m00-m07.md) · [现有线上与历史记录](../../STATUS.md) · [回业务目录](README.md)
