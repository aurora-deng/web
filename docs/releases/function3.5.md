# Function3.5 · 小研记忆、DAG 与网关映射

发布日期为 2026-11-21。比较基线为 `Function3.3` 的提交 `3cff2a97c32706e81ad8cbe977e2e5d940e1549b`。[完整变更文件清单](function3.5-files.md) · [项目文件架构](../architecture/overview/project-layout.md)

## 相对 Function3.3 的更新

| 模块 | 本版交付 |
|---|---|
| 小研偏好与个人知识 | 按账号、站点和使用模式保存表达偏好及个人要点。使用有上限的词法召回、动态提示和输出预算。可按公开故事分类累计浏览兴趣，也支持偏好衰减、手动移除和清空。 |
| 长会话 | 保留当前问题和近期原文，把旧内容滚动压缩成摘要。使用游标记录边界，成功回答后再保存。已确认的历史偏好带有「来自会话压缩」标记。 |
| DAG 运行时 | 按依赖调度节点，设定共享并发、节点超时、总时限、取消和受控重试。写操作在依赖成功后提交，每次运行和尝试生成结构化事件。 |
| 具身协作示例 | 合成的观测、任务、规划和安全审核按工作区及帧号关联，输出待确认提案或等待结果。Observer 可按 Trace ID 查询实际生成的示例事件。 |
| Envoy 网关 | 明确小研页面、静态资源、网页推理和研途的路由归属。Windows 私人推理可以走校验证书及名称的 TLS 上游。启动时对照页面和关键脚本的实际字节。 |
| Observer | 最近历史使用已解码采样，较大查询支持协商 gzip。内置仪表盘增量更新曲线和游标，自定义面板共享相同表达式的请求。 |

[小研进度与使用](../reports/xiaoyan-personalization-progress.md) · [DAG 与具身协作](../architecture/business/ai/agent-workflow-runtime.md) · [Envoy 映射](../ops/runtime/nebula-yantu-community.md#小研路由与资源映射) · [Observer 读取与刷新验证](../observability/validation.md#查询速度与自动刷新)

## 这版代码放在哪里

```text
backend/
  server/                              Linux 通信核心
  application/                         共享业务 API、身份校验和事务
  ai/private/                          站点模型与私人 HTTPS 推理
  study/AccountWorkspace/
    GatewayConfig.py                   研途及前端资源与 Envoy 路由装配
    LabRunner.py                       隔离启动与资源、TLS 核对
    ReleaseBundle.py                   网关及前端交付清单
client/
  assistant/
    LocalAssistantService/             Windows 私人会话和压缩上下文
    PreferenceInference/               账号绑定的辅助模型调用
  companion/ProxyServer/               回环入口与上游站点标识
frontend/
  phase11/shared/
    AgentRuntime/                      DAG 和多 Agent 协作接口
    PersonalAgent/                     记忆、召回、压缩与小研节点装配
    AssistantPanel/                    Windows 私人入口
    WebAssistantPanel/                 网页助手入口和会话历史
  observability/                       指标图表、自动刷新和自定义面板
tools/observability/
  gateway.py                           Envoy 路由与受信 TLS 上游生成
  observer/                            指标取数、传输和网关事件
examples/agent-coordination.mjs         可执行的具身协作模拟
tests/                                 前端、网关、Observer 与 Windows 契约
docs/                                  架构、运行、版本报告和逐文件清单
```

网页调用同源网关。小研先在应用层运行 DAG，再把选定的上下文交给模型。Windows 入口经本机服务组合私人历史与已授权资料片段，私人推理使用验证过的 HTTPS。Observer 从受控来源接收事件或查询指标，不需要读取会话正文。[完整目录树和依赖方向](../architecture/overview/project-layout.md)

## 验收范围

| 项目 | 结果与口径 |
|---|---|
| 前端与浏览器 | 106 项前端测试覆盖依赖失败、取消、重试、会话压缩和账号隔离。Edge 验收了小研长会话、账号切换和 Observer 图表刷新，模型调用使用替身。 |
| 网关 | 8 项契约检查路由先后顺序、同机接口边界、TLS 配置和旧资源检测。新版 Envoy 的虚拟机转发和生产切换单独验收。 |
| Observer | 50 项契约覆盖查询、采集与故障，并通过真实本机 HTTP 验证多 Agent 示例事件。七天 Windows 合成数据的等值读取中位耗时为 167 ms，旧实现为 536 ms。 |
| Windows 客户端 | Release 构建和 5 项原生契约通过，覆盖本机资料、会话保护及权限。 |
| 线上版本 | 2026-11-21 已经用 Windows Edge 打开现有 `18095` 的登录页，确认其资源版本为 `7f8c7f1b31e4435c`。Function3.5 前端资源版本为 `4364119c78f60e44`，发布分支用于源码交付。 |

版本验证使用已有工具链。Windows PowerShell 可以运行以下命令。

```powershell
python -B scripts/project/ci_checks.py source --observer-http --output build-function3/function3.5-source-ci.json
node tests/function3/frontend/personal_agent_browser.cjs
node examples/agent-coordination.mjs retry
git diff --name-status 3cff2a97c32706e81ad8cbe977e2e5d940e1549b..Function3.5
```

[现有入口的启动与回退](../ops/runtime/nebula-yantu-community.md) · [Observer 启动说明](../observability/setup.md) · [当前版本状态](../../STATUS.md)
