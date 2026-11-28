# 项目目录与构建边界

Function3.7 的业务目录已按领域和职责归位；不只整理 AI。新增引用优先使用规范路径，旧薄转发头只用于兼容。[业务目录规范与验收](application-domain-layout.md) · [C++ 系统架构](cpp-systems.md)

```text
main.cpp                              进程入口，只交给 ApplicationRuntime
backend/
  server/                             冻结的通信核心（152 文件）
  application/
    modules/
      accounts/ contacts/ chat/ files/ 账号、联系人、聊天、文件
      messaging/                      Outbox、代理和投递编排
      ai/                             持久化 AI 用例、知识/训练业务接口
      shared/                         公共契约、协议辅助、兼容聚合头
      learning/                       教学路由
      tree/ activities/ admin/ ...    既有社区领域模块
      <领域>/
        domain/ ports/ application/ infrastructure/ transport/ bootstrap/
    bootstrap/                        应用、网络、存储与全局运行装配
    storage/                          共享数据库/内存驱动和事务生命周期
    observability/                    业务操作、指标、Trace 与日志
    config/ runtime/                  配置和有界执行器
    business/ transport/http/         仅保留既有薄兼容头
  ai/
    bootstrap/                        推理依赖装配
    inference/                        contract/inprocess/remote/resilience/metrics
    registry/ rag/ training/data/     模型治理、检索和训练数据
    private/ speech/ teacher/         私人生成、语音、teacher 设计位置
  agent/                              规则、工具/技能、运行时与可执行入口
  fleet/ workflow/ systems/           车队、工作流与独立构建入口
  study/AccountWorkspace/             账号隔离的研途工作区
scripts/
  ai/                                 data/training/export/toolchain
  function3/                          日常运行和隔离验证
  project/                            边界、契约、文档与交付检查
  project/layout/                     显式、可备份的目录迁移
client/                               Windows 客户端
frontend/                             社区、Agent 工作台和 Observer 页面
tools/ai/                             模型与训练维护 CLI
tools/observability/                  网关与分层 Observer（查询/指标/日志/告警等）
robotics/nav2_gateway/                独立 ROS2 适配
```

类型/接口按领域归属，不与驱动混放；共享存储的事务和连接所有权保持不变。具体组件列表见 [迁移清单](../source-layout-migration-3.json)。构建产物、模型、用户上传和真实配置不属于源码整理对象。

## 调用与构建边界

1. `main.cpp` 进入 `ApplicationRuntime`。协议适配把请求交给应用服务，服务依赖领域对象与 Port。PostgreSQL、Redis、RocketMQ 和云语音适配实现这些接口。
2. 社区装配由 `Function3Runtime` 接线。内容模块写入业务数据及同事务事件，投递模块在提交后处理 Outbox。CMake 目标分别为 `function3_community_content` 和 `function3_community_delivery`，由 `function3_http` 在 PostgreSQL 配置下装配。[社区模块说明](../business/engineering/community-runtime-modules.md)
3. Windows 使用 `client/CMakeLists.txt`。浏览器前端位于 `frontend/phase11/`。ONNX gRPC 推理、Kokoro 语音和显式训练或转换任务有各自的进程边界。
4. Linux 两档复现通过同一脚本进入。通信档验证 HTTP、静态资源和未配置业务时的返回，业务档使用一次性 PostgreSQL 跑 CTest 和真实业务冒烟。[复现指南](../../ops/validation/reproduce.md)
5. Observer 独立部署。C++ 业务负责产生指标与 span，Python 侧负责采样、保存、计算窗口和查询，浏览器负责联动呈现。[监控数据流与组件索引](../../observability/architecture.md)
6. Envoy 是可单独启动的入口进程。Prometheus 抓取其原生指标，Observer 读取脱敏 JSON 访问记录并通过采样响应头关联业务 Trace。[隔离网关架构](../../observability/envoy-gateway.md)
7. 研途网关转发 `/api/study/` 到账号工作区，后者向固定 NEBULA 上游验证会话，再按账号选择独立存储。浏览器导航和 Windows 客户端共用这一入口。切换账号经原登录接口处理，研途前端只装载服务端确认的新工作区。[研途运行手册](../../ops/runtime/nebula-yantu-community.md)
8. 小研在对话发起前组装个性化上下文。Windows 的 `PreferenceInference` 使用已绑定的会话将辅助推理发送给自有 Linux AI，资料仍由本机私人空间检索。[小研数据流与验收](../../reports/xiaoyan-personalization-progress.md)

## 文件与数据位置

C++ 组件用同名目录存放头文件与实现。纯接口保留头文件，配套辅助代码随组件保存。源码树上方未展开 `build-*` 等本机构建目录。数据库、上传文件、私钥、云 Key、模型和私人训练资料按各自运行配置存放。

共享业务以 Linux PostgreSQL 为权威存储。Windows SQLite 存放按账号隔离的私人资料。数据库升级只追加编号迁移，原库变更和日常服务切换按运维文档操作。[迁移规则](../../database/migration-policy.md) · [日常运行](../../ops/runtime/function3-runtime.md)

[文档导航](../../README.md) · [当前端到端图](../business/flows/current-end-to-end.md) · [代码索引](../business/indexes/README.md) · [历史目录迁移清单](../source-layout-migration.json)

Observer 的 29 个实现模块也已从平铺目录归位，保留原服务 CLI 的薄兼容入口。[Observer 职责目录](../../../tools/observability/observer/README.md) · [精确迁移清单](../source-observer-layout-migration.json)

监控配套工具的 23 个实现也已按网关、实验、依赖供应、诊断、验收和留存分组，20 个原命令名保留，避免破坏进程身份校验。[工具目录与边界](../../../tools/observability/README.md)

应用层观测实现已按 catalog、logging、metrics、operations、tracing、runtime 分组，详见 [应用观测目录规范](application-observability-layout.md)。

根层通用脚本按 [benchmarks、reports、testing、diagnostics](../../../scripts/README.md) 归类，保留 9 个历史命令的薄转发；新引用使用职责目录。迁移检查见 [精确清单](../source-script-utilities-migration.json)，不执行删库、停服务或进程清理。
