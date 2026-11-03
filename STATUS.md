# 当前开发状态（文档整理：2026-11-03；功能验收：2026-10-31）

文档已按主题分类，入口见 [文档导航](docs/README.md)、[Phase时间线](docs/roadmap/stage-index.md)。本次不改变功能和既有验收结果；源码与文档在 `feature/function3-tree` 分支维护，GitHub同步不代表部署日常服务。

Phase49完成本轮接口对齐、领域资源装配与数据库对象分层。**功能回归通过，严格性能门槛仍未通过。**

- 本地分支 `feature/function3-tree`；本轮前检查点 `42b3f8f`，Phase48与更早历史报告保留。
- 通信核心152份文件不变；main由964行/37个unique_ptr缩成14行进程入口，资源归各领域Runtime管理，显式幂等停止。
- 119项HTTP方法/路径、18个领域绑定；106处前端字面路径迁移。字段、权限、WS/SSE和Windows本地接口仍有各自契约，路由目录不冒充完整Schema。
- 账号、联系人、聊天、文件服务与前端控制器按领域归类；旧头文件与少数兼容委托保留。用户搜索独立归联系人目录。
- 数据库明确Query/Command、业务对象、驱动内部行、公开投影；复用原事务和连接，不新增数据库或迁移（仍024）。
- Linux三种矩阵各115项；前端33项、工程14项、七组浏览器、Windows两种构建各5项通过。10组真实业务、真实Windows代理、独立Worker故障与12项装配失败检查通过。
- 完整三轮：/fast中位-3.89%，混合吞吐中位-16.93%，错误0；ACK第3轮8.710→15.545ms（+78.47%）超标。中断的一轮单独保留，未采用较好记录替代。
- 聚合指标指向该轮SQL与投递同时变慢，没有队列满/连接池超时；逐消息COMMIT→ACK发送因果定位仍待补齐，不据此宣称重构提速。
- 未安装新依赖、未写原业务库、未替换日常服务；本次同步源码与文档不改变这些运行边界。云Key、语音自然度、远程CI与生产容量的历史限制保留。

入口：[改动与验收](docs/architecture/business/engineering/phase49.md)、[接口接入教学](docs/architecture/business/guides/phase49-teaching.md)、[数据库分层](docs/database/domain-layering.md)、[代码索引](docs/architecture/business/indexes/phase49-code-index.md)、[整体调用图](docs/architecture/business/flows/phase49-end-to-end.md)、[验证与回退](docs/ops/validation/phase49-validation-and-rollback.md)。
