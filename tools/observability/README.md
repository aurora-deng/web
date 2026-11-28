# 监控与隔离验证工具

实现按职责归位；这里的整理不改变认证、网络隔离、进程所有权和运行预算，也不表示已运行安装或实验。

| 目录 | 职责 | 主要输入 / 输出 |
| --- | --- | --- |
| [observer/](observer/README.md) | HTTP、查询、指标、日志、资源、告警与存储 | 有界事件、配置 / 观测响应与隔离存储 |
| [gateways/](gateways/README.md) | Envoy 配置渲染与受管进程适配 | 模板、显式端口 / 配置与受管生命周期 |
| [laboratory/](laboratory/README.md) | 隔离实验编排、场景、探针与预览 | 明确的依赖、运行目录 / 合成数据与测量记录 |
| [pipeline/](pipeline/README.md) | 已审批的依赖供应 | 固定包与校验 / 隔离依赖目录 |
| [diagnostics/](diagnostics/README.md) | ACK、函数追踪、实验对比与分析格式 | 隔离证据 / 诊断与分析输出 |
| [verification/](verification/README.md) | 网关、告警、指标、双节点与综合验收 | 隔离实验记录 / 有界验收结果 |
| [lifecycle/](lifecycle/README.md) | 显式运行标记与留存策略 | 运行目录、保留规则 / 标记与可审阅清理结果 |
| gateway_plugins/ | 有独立构建边界的网关扩展 | 扩展源码 / 显式构建产物 |

## 旧命令为何保留

原平铺的 23 个 Python 模块只保留薄兼容入口，20 个既有 CLI（命令行入口）的名称和退出码语义不变。例如帮助仍通过：

```sh
python -m tools.observability.lab --help
python -m tools.observability.isolated_gateway --help
python -m tools.observability.verify_gateway --help
```

新 Python 引用使用规范实现路径，如 `tools.observability.gateways.gateway`。旧路径导入和新路径导入指向同一个模块对象，不复制实现，因此既有 mock 目标仍能作用到真实函数。

**保留命令名是安全兼容要求，不是重复业务代码。** 进程身份校验会核对原 `tools.observability.lab` 等命令；不能只为路径整齐就替换这些字符串。实验启动和留存工具仍须按原有权限、预算和隔离规则显式使用。不要将实验目录加入正式运行包。运行包只增加网关实现及其空初始化文件，保持供应范围最小。

## 验收与回滚

23 个实现的可执行 AST（抽象语法树）在排除 import 与等价源码根定位后保持一致；CLI 字符串、进程身份、安装目标与清理规则均保留。

```sh
python tests/project/test_observability_tools_layout.py
python scripts/project/ci_checks.py source
```

迁移清单见 [精确记录](../../docs/architecture/source-observability-tools-migration.json)。迁移工具默认只读，应用前必须审阅摘要并验证独立备份；已应用后拒绝重放。回滚先比较备份和后续改动，不自动覆盖或递归删除。
