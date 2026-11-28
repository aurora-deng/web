# Observer 职责目录

原来平铺在本目录的 29 个 Python 实现已按职责归类。外部地址、认证规则、指标名、API 路由、数据库结构和留存策略未在这一步改动。

| 目录 | 输入与输出 |
| --- | --- |
| `contracts/` | 校验固定事件契约，读取版本化业务操作目录 |
| `queries/` | 将有界查询参数交给指标/Trace/实验查询，不管理 HTTP 连接 |
| `metrics/` | 处理本机指标、直方图、测量质量与 SLO 结果 |
| `traces/` | 读取、筛选本地 Trace 并形成调用视图 |
| `logs/` | 规范化结构化日志与网关访问记录，形成查询结果 |
| `resources/` | 将受限资源采样与提交身份形成历史记录 |
| `alerts/` | 固定/自定义告警规则和受控求值 |
| `sources/` | 访问经配置且有访问校验的数据源，不扩大允许地址 |
| `storage/` | 现有有界事件存储与事务实现 |
| `configuration/` | 工作区与数据源配置的校验和持久化 |
| `analysis/` | 事件投影、对比和成本分析 |
| `http/` | Observer HTTP 服务和访问预检 |

`server.py` 只保留旧 CLI 的薄入口，`python -m tools.observability.observer.server --help` 仍可使用；实现位于 `http/server.py`。模块导入不等于启动服务。实验、安装、网关和采样命令仍在上一级各自的入口，不因为整理目录而自动运行。

## 验证与限制

```sh
python tests/project/test_observer_layout.py
python tests/project/test_observer_queries.py
python scripts/project/ci_checks.py source
```

迁移以改前备份、路径边界和计划摘要为前提。29 个模块的执行 AST（抽象语法树）在去掉导入和同义的源码根路径定位后必须与改前一致，避免把协议或存储行为变化混进搬家。测试还会导入全部模块、验证新包无启动副作用，并确认旧服务入口引用同一个 `ObserverServer`。

使用现有 Python 标准库和项目既有后端依赖，不安装新包。上述源码与导入测试不代表真实浏览器、外部采集后端或正式服务的验收。[精确移动清单](../../../docs/architecture/source-observer-layout-migration.json)
