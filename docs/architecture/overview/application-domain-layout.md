# 业务目录规范与迁移验收

本轮依据原优化计划 P2 4.3 和用户 2026-11-27 的补充要求，将非 AI 业务的混合目录也按职责归位。这里只记录本地源码重构，不代表已部署或性能验收完成。

## 划分规则

先按业务领域，再按技术职责：`backend/application/modules/<领域>/<层>/<组件>/<同名文件>`。

| 层 | 放什么 | 不放什么 |
| --- | --- | --- |
| `domain/` | 领域类型、值对象和失败分类 | HTTP、SQL、模型驱动 |
| `ports/` | 业务接口、仓储和外部能力契约 | 数据库连接、驱动实现 |
| `application/` | 用例编排与业务规则 | 服务进程入口和协议解析 |
| `infrastructure/` | 密码编码、持久化和外部适配 | 未鉴权的管理入口 |
| `transport/` | HTTP、WebSocket、SSE 和序列化适配 | 事务边界和核心领域规则 |
| `bootstrap/` | 该领域的依赖装配和生命周期 | 其他领域实现的大杂烩 |

目录深度不是目标：没有对应职责时不创建空目录，不把一个正确的小组件拆成多个空壳。

## 已归位的范围

- `accounts/`：账号类型、认证业务端口、凭据/限流端口和实现、认证 HTTP、身份装配。
- `contacts/`：联系人类型与仓储端口、社交业务端口、联系人装配。
- `chat/`：会话/消息类型、仓储和聊天业务端口、实时适配、消息 JSON、聊天及实时装配。
- `files/`：文件类型、仓储/存储/业务端口、文件 HTTP、文件装配。
- `messaging/`：Outbox 类型、仓储端口、投递编排。保留共享事务归属，不移动成新的数据库连接。
- `ai/`：需要用户权限、聊天历史、审计持久化的 AI 用例、知识/训练业务端口和 HTTP。`backend/ai/` 仍负责推理引擎、RAG、模型治理、训练与私人生成，不与业务用例混为一谈。
- `shared/`：真正共享的 ID、事务端口、错误、HTTP 辅助与兼容聚合头。
- `learning/`：教学示例路由，与账号聊天等持久化业务分开。
- `tools/ai/ModelAdmin/`、`tools/ai/TrainingAdmin/`：模型与训练命令行工具；没有改变执行权限。

合计 66 个组件/工具路径、87 个原有源文件迁移。`BusinessInterfaces.h` 中 13 个已有类型与接口拆到 12 个领域头文件，定义逐字节比对；聚合头只负责装配兼容，不再包含定义。每条移动见 [迁移清单](../source-layout-migration-3.json)。

## 保留的边界及兼容入口

- `backend/server/` 的 152 个冻结文件不变。
- 顶层 `application/bootstrap/` 保留应用、网络、全局业务、存储和运行管理装配；并非所有叫 Runtime 的文件都要归同一目录。
- `application/storage/` 的 PostgreSQL / 内存适配保持共享事务和连接生命周期，没有为目录统一擅自拆连接池或改 SQL。
- 既有 `business/{AuthService,ChatService,FileService,SocialService}` 和旧 HTTP 转发头仍是三行兼容入口，不含重复业务实现；新代码直接引用领域内的规范路径。
- `agent/program/` 是可执行入口，`agent/application/` 是用例；不为统一名字而将 CLI 当作装配层。`backend/systems/` 保留可独立构建入口，数量少不是删除模块的理由。
- `study/AccountWorkspace/` 按账号边界协作，当前未发现必须改动其外部入口的理由。
- 不重写 `docs/evidence/`、历史报告、性能证据和旧迁移清单；它们描述当时版本的路径。

## 验收与回滚

```sh
python scripts/project/check_boundaries.py
python tests/function3/architecture/check_boundaries.py
python tests/project/test_application_layout.py
python scripts/project/ci_checks.py source --output build-function3/application-layout-source-ci.json
```

自动检查移动唯一性、引用和 CMake 路径、13 个业务定义的原始哈希、API 方法/路径/操作标识不变。路由目录的 `sources` 字段允许随文件路径迁移，不能误报为协议变更。前端资源清单和原生技能生成物必须保持原字节。

迁移前备份位于工作副本外的验证父目录，应用前校验完整备份、计划摘要和并发修改；出现冲突立即停止，不使用强制覆盖、递归删除或 Git reset。回滚应先审阅备份与后续改动，不自动整包覆盖。

源码检查或本地头文件检查通过，不等于 Linux 全量编译、TLS 链接、真实数据库压测或浏览器验收通过。目录重构本身不宣称性能收益。
