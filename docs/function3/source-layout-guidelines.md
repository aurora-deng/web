# 源码目录规范与分阶段迁移

核对日期：2026-11-27。本文记录目录规范和实际迁移范围；C++ 路径迁移已在显式备份后执行，正式服务启动方式未变。
当前项目工作副本为 `Function3.7`；本地证据目录不是另一个实现版本。

## 当前结构与命名约定

- `backend/<领域>` 表示业务边界。现有领域包括 `application`、`ai`、`agent`、`fleet`、`workflow`、`study`、`systems` 和 `server`。
- C++ 组件沿用现有 `ComponentName/ComponentName.h + .cpp` 风格；Python 适配器和脚本保留现有 snake_case 命名，不为统一外观改公共接口。
- 领域内部先区分 `domain`（规则）、`ports`（外部能力契约）、`application`（用例）、`transport`（协议适配）、`bootstrap`（组装启动）。不是要求给每个领域创建全部空层。
- Agent 工具与技能是一项一目录；声明、实现、说明并列，显式清单控制加载。事务、权限、确认、回执、审计仍留在运行时 / 适配器。
- `scripts/ai` 按数据、训练、导出、工具链划分；迁移保留旧 CLI 兼容入口。按训练方式拆分时，只创建真实存在的方法，不把预留目录写成已实现能力。
- `tests` 按现有领域和层次放置。fixture 不含真实账号、会话或模型权重；源码契约测试、编译测试、服务集成和性能实测分别报告。
- `docs/function3` 保存当前功能和迁移边界；原始证据留在证据目录，不能为了文档一致而重写旧报告。

## AI C++ 迁移清单（已迁移，Linux 重编与 8 组回归通过）

| 当前组件 | 候选目标 | 保持不变的边界 |
| --- | --- | --- |
| `models/ModelProvider` | `inference/contract` | 公共类型、回调和生命周期 |
| `models/InProcessOnnxModelProvider` | `inference/inprocess` | 已批准模型根、队列和线程约束 |
| `models/GrpcOnnxModelProvider`、`GrpcOnnxWorker`、`OllamaModelProvider`、`OllamaStreamDecoder` | `inference/remote` | 固定端点、鉴权、超时、取消和 TLS |
| `models/ResilientModelProvider` | `inference/resilience` | 故障处理与降级语义 |
| `models/ModelRegistryService`、`ModelEvaluationService`、`DeterministicExactScorer` | `registry` | 激活授权、质量门槛和旧版本回滚 |
| `models/RagService`、`DeterministicKnowledgeIndex`、bootstrap 的 KnowledgeRuntime | `rag` | 检索范围、来源和大小约束 |
| `training` 内的数据服务 / JSONL 导出 | `training/data` | 数据授权、校验和 lineage |
| `teacher` | 仅 README | 不新增行为 |

当前组件已移到表中对应的领域目录，仍保留 ComponentName/ComponentName.h + .cpp 结构。显式清单为 `docs/architecture/source-layout-migration-2.json`：15 个目录、54 个相关文件，旧路径不保留第二份 C++ 实现；代码、测试和 CMake 引用同步更新。
`speech`、`private` 不动；冻结的 `backend/server` 不纳入本次目录迁移。

## 迁移流程

1. 迁移前记录 Git 状态、相关文件字节摘要、公共契约及生成物；保留用户未提交工作。
2. 为每个“旧路径 → 新路径”建立显式清单；先核对全部 include、CMake 源文件登记、安装 / 打包列表、测试和边界检查，禁止全仓盲替换。
3. 大范围移动先确认范围和回滚方案。将结构调整与行为 / 性能调整分开验收，避免无法归因的混合改动。
4. 移动后检查清单无遗漏，行为和生成物保持一致；必须通过 Linux 编译、相关 CTest / Python / 前端测试，不能只通过 Windows 语法检查。
5. 包内路径与运行时导入路径一起验收；成功打包不等于成功部署。未保存的本地源码不得以旧二进制测试记录冒充已验证。
6. 只有新版本独立验收、回滚所需版本已保留并且清理清单获批，才删除被替换的旧目录。

## 其它模块待办（仅登记，不批量改动）

- `application` 现有分层 / 按模块组织的并存，先确认依赖方向，再决定是否统一；不能因目录名字不一致重写边界。
- Agent 现有 `program` 和其它领域 `bootstrap` 的命名差异，不影响行为时优先保留，未来按明确清单调整。
- `systems` 目录的归属与扩展需求待确认，禁止为“整齐”合并到不相干领域。
- AI 工具脚本已按 export / training/lora / toolchain 整理，并保留旧 CLI；迁移清单与字节对照见 `docs/architecture/source-ai-script-migration.json`。不借迁移安装依赖或下载模型。
- 独立推理 Worker、AI 准备执行器、数据库事务和索引优化属于行为变更，不属于纯目录整理，应分别设计、测试与审批。

本文不授权远程上传、服务变更、数据库迁移、删除或 Git 提交。

## 2026-11-27 迁移验证

- 执行前在证据父目录保存完整字节备份，预演摘要一致后才移动；无 Git reset/clean/commit/push。
- `test_ai_cpp_layout.py`：15 个组件新路径 / 旧路径无重复、include 解析、CMake 引用 3 项通过。
- `scripts/project/check_boundaries.py`：冻结 152 个通信核心文件与依赖边界检查通过。
- AI 脚本迁移契约 4 项、导出合成契约 7 项、Worker mock 契约 8 项通过；不代表真实模型训练或正式部署。
- Linux 隔离副本已按相同清单迁移，webserver / Worker / smoke 重编通过；8 组 CTest 连续三轮通过（r6，24 次）。历史阶段结果正文保留原路径；普通文档的代码链接仅重定向到现存新位置。
