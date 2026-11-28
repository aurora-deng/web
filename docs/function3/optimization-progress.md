# Function3.7 优化进度与验收边界

核对日期：2026-11-27。状态不是完成百分比，而是实现、测试、部署三条独立证据链。

| 项目 | 已完成 | 尚未完成 |
| --- | --- | --- |
| PostgreSQL | AI 历史查询限制 64 条；17 项参数快照；隔离实例身份校验；WAL / IO / HOT 聚合观测及离线调参判定 | A/A 启动因 TLS 依赖混用 SIGSEGV，待修复后重测；有效 A/B / WAL 对照、正式参数和事务 / 索引决策 |
| 0.5B 模型 | 同条件三轮质量 / 首段 / 速度 / RSS 共 186 次生成；真实训练→ONNX Adapter→C++ 推理烟测通过 | 质量未达切换门槛，保持默认模型；普通聊天干扰和绑核收益尚待测量，烟测不代表训练质量提升 |
| Envoy | ALPN/TLS 1.3 修复；新增 TLS 上游预检；已获批修正 VM 上游 CA 路径并仅重启 Envoy；HTTP/HTTPS/原生 TLS 各 40 次健康请求均 200 | h2 / HTTP1 与连接复用已验证，限定窗口 overflow 为 0，不调整熔断；历史页面连接提前终止根因及全站验收仍未完成 |
| 前端 | 长缓存 / 304、CSS 独立 link、背景图移出 JS、modulepreload；图标 714901→15435 字节；图片字节参与统一版本；JPEG 路由已 Linux 编译 | 真实浏览器首屏 / 二次加载和正式部署验收 |
| Agent 目录 | 18 个工具的声明与两种数据源实现、19 个技能配方按项拆分；真实 Storage 合成事务测试 | 新隔离源码副本之外的正式同步和线上验收 |
| AI/训练目录 | 脚本分层 / training/lora / 兼容 CLI；15 个 C++ 组件与 54 个引用文件迁移；Linux 局部回归；真实训练 / 导出 / C++ 闭环 | 覆盖最新修复的完整回归、正式部署和线上验收 |
| 控制台启动 | 已定位日常 Web 启动丢失控制台参数；恢复原有 root allowlist，新增非敏感持久配置并仅重启 Web；已注册入口匿名 401 | 未使用 root 密码或会话，登录后页面渲染尚未验收；Windows 原有本地转发端口连接超时 |
| 发布 | 开发代码已上传新建 optimization 隔离副本，主服务与 Worker 编译并完成局部三轮 CTest；不覆盖正式版本 | 正式部署、Git 提交 / 推送、旧历史目录清理仍未执行 |

## 优先问题：Envoy 上游证书漂移

2026-11-27 的只读诊断：HTTP 网关健康正常；HTTPS 网关对 Host=127.0.0.1:18495 的健康请求冷连接 20/20、复用 16/16 返回 503。
原生 TLS 用当前服务公开证书校验返回 200，用 Envoy 当前上游 CA 校验则失败（自签证书不在该信任集中）。
网关信任 `function3-7-79549b8/runtime/localhost.crt`，服务重启后使用 `function3/runtime/localhost.crt`，公开指纹不同。
这说明“之前 TLS 修复通过”并不能证明重启后的实时路径仍然可用。失败响应的耗时不计作成功请求的性能改善。

新增 `scripts/function3/validation/gateway_tls_preflight.py`：仅对配置中的固定回环上游做证书、精确 SAN 和 ALPN 验证，失败即返回非零。
它不读取私钥，不发业务请求，不修改信任或配置，不重启服务；不是业务 readiness 检查的替代品。
修复已于 2026-11-27 09:23（Asia/Shanghai）获得当前明确批准并执行：备份并仅将 Envoy 上游 CA 指向已确认的当前服务公开证书，保留 SAN/ALPN/TLS 校验，校验配置后仅重启网关并复测。
不覆盖或重建任何私钥、不改用明文上游、不关闭证书验证。

### 已批准修复的现场验收与剩余问题

- 修改 `function3-7-79549b8/runtime/envoy.json` 中唯一的 `nebula_application_tls` 上游 `trusted_ca.filename`，从发布目录公开证书改为当前 Web 的 `function3/runtime/localhost.crt`。与备份比较，仅一处文本替换、一处语义字段变化；权限仍为 `0600`。
- 候选及实际配置均通过现有 Envoy 的 `--mode validate`（退出码 0）。Envoy PID `37028 → 56469`，只发送 SIGTERM，没有强杀；Web `54109`、Workspace `37027` 的 PID/startTicks 均未改变。未操作数据库。
- 远端备份：`/home/pikachu/phase11-data/function3-7-79549b8/runtime/envoy-ca-backup-20261127T092332+0800/`，保留原配置、部署元数据、校验日志和修复回执。旧配置本身存在已确认的证书错配，不代表恢复旧配置即恢复健康。
- HTTP 网关、HTTPS 网关、原生 HTTPS 各 40 次 `/health/ready`（20 次冷连接、4 次会话首请求、16 次复用请求）全部 200。HTTPS 验证启用，协商 TLS 1.3 / HTTP/1.1；旧 CA 仍不能验证 Web 当前证书，负向校验生效。前门证书及回环监听地址未改。
- 短样本中 HTTPS 网关响应头到齐时间 P50：冷连接约 `49.048 ms`、复用约 `1.380 ms`。这不是精确首字节测量、容量测试、AI 推理指标或可归因的优化收益；旧 503 耗时不能作成功响应基线。
- 扩展验收曾在 HTTPS `/phase11/` 记录一次 503，Envoy 诊断为 `upstream_reset_before_response_started{connection_termination}`，并非该次证书校验失败。之后 HTTP/HTTPS 各 6 轮低频“健康 → 页面”请求全部 200，但连接提前终止根因未确认，不能标为全站稳定。
- 匿名权限页面仍为 401；`/phase11/agent-control/` 和 `/api/private/agent-control/access` 在 HTTP/HTTPS 网关及原生 Web 均返回 404。控制台启用状态/路由需继续核对，不能用本次 CA 修复宣称 root 控制台已恢复。未读取登录凭据或执行登录写操作。
- 该轮 CA 修复的批准不包含 Web/PG 重启、其它线上配置修改、模型切换、清理、Git commit/push 或本轮开发代码部署，故该轮均未执行。该轮整体页面扩展验收结果保留为失败。随后单独获批的控制台修复记录见下节，不覆盖原失败证据。

本地证据（相对 `validation-runs/20261126-function37/`）：
`envoy-ca-prechange-audit-20261127.json`、`envoy-ca-repair-result-20261127.json`、`envoy-ca-postrepair-probe-20261127.json`（扩展验收失败）、`envoy-page-isolation-20261127.json`、`envoy-page-access-diagnosis-20261127.json`、`envoy-ca-lowrate-confirmation-20261127.json`。

## 后续批准：控制台 404 与 Web 启动配置持久化

2026-11-27 09:31 的现场检查确认：Web 二进制含控制台功能，日常工作目录也有控制台资源，但当前进程缺少 `WEB_AGENT_CONTROL_ENABLE`、`WEB_AGENT_CONTROL_USER_IDS`、`WEB_AGENT_CONTROL_APPROVER_IDS`。C++ 在 enable 为假时不注册控制台路由；日常启动脚本此前不持久加载已授权的控制台设置。原有部署记录和访问策略仍明确给 root 相同的操作员/审批员 ID，本次没有修改访问策略、账号或角色。

已获当前明确批准并于 09:42 执行：

- 为 `scripts/function3/runtime/service.py` 增加 `control_config.py` 加载器，从运行目录 `agent-control.json` 仅加载启用状态及明确的现有操作员/审批员 ID，不读取密码、私钥或环境文件。运行配置为 `0600`，拒绝重复字段、未知字段、过大输入、非法/重复 ID 和符号链接；Linux 检查文件所有者及可写权限。缺失文件保持原兼容行为，存在但无效则拒绝启动。
- 仅同步上述两个启动脚本；候选夹具测试留在备份目录。没有同步此前未完成的其它模块，没有调用会检查数据库/生成证书/复制前端的完整日常部署入口。
- 备份原启动脚本、PID/ticks、部署元数据后，以相同 Web 二进制、工作目录、日志、明确的非秘密环境设置及原密钥文件引用启动。只优雅停止 Web `54109`，新 Web `57136`；Envoy `56469`、Workspace `37027` 不变。PID/ticks 和 deployment 记录已对齐。
- Web 当前公开证书未变，Envoy 配置未变；未运行数据库命令、未写账号角色、未切换模型，未强杀进程。
- 原生 HTTP/HTTPS 及网关 HTTP/HTTPS 的健康与首页检查均 200；控制台已注册入口、JS/CSS、访问 API 和权限页在匿名访问下均 401。规范控制台入口是 `/phase11/agent-control/`；直接 `.../index.html` 并非已注册别名，仍为 404。额外别名探测造成的测试失败证据保留，没有为测试放宽认证或增加公开静态路由。
- 重启后网关 HTTP/HTTPS 各 6 轮“健康 → 首页 → 健康”均 200；24 项已注册受保护路径检查全部符合 401。本次有界日志尾部检查未见重启后的新 5xx，但不据此声明修复了此前连接提前终止的根因或完成全站压力验收。
- 离线源码 CI 37 组通过，Windows observerHttp 未请求。新增配置测试在 Windows 为 6 通过 / 2 平台条件跳过；Linux VM 8 项全部通过；现有控制台路由测试 6 项通过。
- 不访问 root 密码、Cookie 或登录令牌，所以 root 登录后的权限页/控制台渲染还需用户正常登录验证。Windows 本机 `127.0.0.1:18095`、`:18495` 在沙箱外复核仍为连接超时；没有擅自新建 SSH 转发、改监听范围或网络规则。不能把 VM 内链路成功等同于 Windows 浏览器已可访问。

远端备份：`/home/pikachu/phase11-data/function3/runtime/control-startup-backup-20261127T093905+0800/`。
本地证据（相对 `validation-runs/20261126-function37/`）：`control-web-prechange-audit-20261127.json`、`control-web-prepare-result-20261127.json`、`control-web-restart-result-20261127.json`、`control-web-postrestart-probe-20261127.json`（保留额外 index.html 别名检查失败）、`control-web-registered-route-probe-20261127.json`、`control-startup-source-ci-20261127.json`。

## 本轮离线验证入口

```text
python tests/project/test_agent_catalog_layout.py
python tests/project/test_control_startup_config.py
python tests/project/test_model_selection.py
python tests/project/test_frontend_graph.py
python tests/project/test_postgres_tuning_gate.py
python tests/project/test_gateway_tls_preflight.py
python scripts/project/generate_skill_catalog.py --check
python scripts/project/frontend_graph.py
python scripts/project/ci_checks.py source
```

依赖图是字面量源码分析，不是浏览器测量。当前结果为 74 个静态 JS 模块、388523 字节（背景图移出后的一次离线快照，并非网络实测）、入口在内 7 层（6 次依赖跳转）。
不能据此宣称真实请求数、TTFB（首字节时间）、缓存命中率或加载速度已经改善。

发布包白名单已补齐 Agent 清单、运行时 Python、逐项声明/配方和必需文档；不包含 `.env`、私钥或缓存。
Agent 契约、工具实现与配方首次加载后只缓存校验过的对象，请求期间不重复扫目录。
原子写入、幂等回执、来源新鲜度、确认和审计仍由现有 runtime 与数据源适配器负责。
更改清单或实现后需重启进程，不做不受控热加载。

## 模型验证的参数化

`final_checks.py`、`ai_mixed.py` 接受：

```text
--model-root /home/pikachu/phase11-models
--model-dir adapter-learning/qwen2.5-0.5b-instruct/olive-int4-adapter-deploy
--adapter adapter_weights.onnx_adapter
--label qwen05-smoke
```

以上仅展示参数，不代表本轮运行了集成测试。`final_checks.py` 还会启动/停止隔离测试服务和写测试数据库，执行前必须确认测试集群、资源预算及远端同步权限。
默认仍是 Phi-3，不会自动激活日常数据库的模型；模型与 Adapter 必须留在指定根目录内。
`ai_mixed.py` 会比对测试种子的模型相对路径和 Adapter，不能只换报告名字冒充测了另一模型。
此基准按 `ai.token` 事件计数，不能在未确认事件与 tokenizer token 一一对应前称为精确 decode token/s；它也不是质量评测。

## PostgreSQL 调参验收

现有 A/A 工具要求至少 10 轮、每轮每组至少 1000 个 ACK 样本，并核对二进制、配置、工作负载和重置夹具摘要。
新增判定器在此基础上接受显式 `postgres-tuning` 模式的至少三轮 A/B：

- 每轮 ACK P99 改善至少 20%，并大于 A/A 的描述性 P95 差值；
- 每轮 `/fast` QPS 退步不超过 10%，错误为 0；不丢弃离群轮次；
- 两组 `fsync`、`synchronous_commit`、`full_page_writes` 必须为 on；
- PostgreSQL 配置摘要必须与对应样本绑定，工作负载和应用二进制不得变化；
- 不修改旧验收结论；输出不代表已修改、已验证线上配置或证明因果。

输入文档包含 `calibration`（原 A/A 原始 summary）和 `performance`（带 PostgreSQL 参数快照及逐轮摘要的新 A/B 数据）。
`performance` 必须包含 `postgresSettings.baseline/current`，每行样本包含对应 `postgresSettingsSha256`。
判定器只读输入，只创建新的结果文件，不连接数据库，不执行 reload/restart，不替操作者决定参数。
现在 benchmark harness 已接入逐轮 before/after 观测和严格判定。r23 / r27 已尝试真实隔离 A/A，但服务在就绪前 SIGSEGV，没有有效性能样本，不得称为校准或 A/B 完成。旧报告若只有 digest，缺完整观测，必须拒绝，不伪造补齐。

```text
python scripts/function3/validation/postgres_tuning_gate.py --input evidence.json --output new-gate-result.json
```

原计划中的 512MB 缓存、15min 检查点与 1000us 提交延迟只是候选方案，不是推荐直接套用的现状。
本次只看到了磁盘配置的 128MB；没有查询线上 `pg_settings`，不能声称核实了全部运行时有效值。

## 必须另行确认的操作

1. VM 上传本轮源代码以及启动隔离模型/数据库压测（不触碰日常业务库）。
2. 如 A/B 达标，备份配置后应用指定 PG 参数；任何 PG 重启须单独确认维护窗口。
3. 默认模型激活、gRPC Worker 接入及正式服务重启部署。
4. Git commit / push / 标签处理；不移动已发布标签或强推。
5. 删除旧目录、停 MySQL、改 GNOME/系统服务：不能由“清理”一词自动扩大授权。

[Agent 目录说明](../../backend/agent/README.md) · [合成数据生成器](../../scripts/ai/data/README.md)

## 2026-11-27 继续实现：本地证据，不代表部署

以下分阶段记录保留各阶段的验证边界；早期的“未执行”仅描述当时状态。2026-11-27 最新状态以顶部总表及文末实测记录为准。原有未提交工作已保留，仍未 Git commit / push、下载模型或部署本轮正式版本。

### PostgreSQL 测量保护

- `postgres_settings.py` 固定采样 17 项 `pg_settings`，每轮测量前后保存数值、单位、上下文、来源、带时区时间和摘要。
- 任意迁移 / 测试夹具重置前检查固定 marker，并只读核对数据库名、用户、data_directory、端口、Unix socket 目录及 socket-only 监听方式；仅凭相似库名不能继续。
- 连接字段和 session options 使用白名单，拒绝密码字段、额外地址和重复 options；psql 不读取 `.psqlrc` / `.pgpass` / service 文件，不继承 PG 连接覆盖变量。元数据查询在只读事务中执行，带 statement / lock timeout 及进程超时。
- `--postgres-tuning` 目前**仅支持连接级 commit_delay / commit_siblings 对照**，不支持也不自动应用全局 shared_buffers / checkpoint_timeout 候选配置。
- A/A 校准必须和 A/B 使用同一二进制、夹具、配置与工作负载。A/B 若误用默认 1200 operations 而校准是 4000，会在测试数据库写入前拒绝；不能事后混用两组结果。
- 判定器要求校准和测量的每一轮都有完整 before/after 观测，拒绝只有摘要、参数漂移、待重启、关闭持久性、伪造来源、无时区或逆序时间。历史报告缺字段时保留原文件并重做，不自动补值。
- `--source` 可指向明确的隔离源码快照；默认路径不变。以上仅完成代码和离线测试，**没有执行基准或测试库写入**。

### 前端与 Agent

背景图片由 434299 字节的 base64 JS 改为 359 字节的同源 JPEG 元数据模块，图片 SHA256 和字节保持不变。
五个 modulepreload 的静态预算小于 64KiB；不预加载图片或可选模块。离线静态图仍是 74 模块 / 7 层，不能声称真实浏览器瀑布已经完全拉平。
JPEG MIME / 版本缓存的 C++ 修改尚需 Linux 编译与实际路由验收；登录后权限页和控制台仍未使用 root 会话验收。

18 个工具实现已拆分到各项 `impl.py`。新增真实 Storage + 合成 SQLite 测试，验证全部读入口、三个原子写入、回执重放、并发同键、失败 / 过期回滚和保留用户编辑的撤销。
没有变更工具契约、扩权或把事务控制下放到工具实现。

### AI 脚本与仍待完成的工作

训练和导出实现移至 `scripts/ai/training` / `export`，旧路径保留兼容入口；共享源模型清单和训练 lineage 校验。
导出子进程改为必要环境白名单，并禁止隐式 Hub token / 遥测，保持离线标记。
合成契约测试不是一次真实训练或导出；本轮没有切换默认 Phi-3。
详见 [AI 脚本说明](../../scripts/ai/README.md)。

原计划尚有实质代码工作，不能统称为“只剩部署”：

1. P1：准备分段、token-ID、模型加载 / 排队、独立执行器 / Provider 队列指标已实现并完成局部 Linux 合成验收；真实模型 / 线上指标仍待验证。
2. P2：AI 脚本 / LoRA、C++ 目录、CMake / 边界引用与规范已整理；迁移后完整 Linux 回归进行中，其它模块仅登记规范，不批量移动。
3. P3：独立 AI 执行器、readiness 恢复与可选 TLS/token Worker 生命周期已实现，局部 Linux / mock 验证通过；真实隔离模型和正式启用未做。
4. P0 / P4：真实模型质量 / TTFT / tokenizer token/s / RSS 对照；PG A/A / A/B 与 WAL / HOT 证据；仅根据证据决定全局参数、事务或索引修改，不盲目套候选值。
5. 正式切换、浏览器权限页面验收和旧目录 / 无用服务清理；删除清单、维护窗口、回滚范围均需确认。

本阶段离线验证命令：

```text
python tests/project/test_postgres_settings.py
python tests/project/test_postgres_tuning_gate.py
python tests/project/test_nebula_tool_adapters.py
python tests/project/test_ai_pipeline_contracts.py
python scripts/project/ci_checks.py source
```

本轮完整检查证据位于工作区 `validation-runs/20261126-function37/`，不纳入源码发布包。
Windows 回环 HTTP 未请求的检查必须单列为未运行，不得计作成功；本地单元测试也不替代 Linux C++ 构建或浏览器性能验证。

补充：按原计划 F1，将首页 `AnnouncementPlayer.css` 从 `styles.css` 的顶层 `@import` 改为 HTML 独立 link，仍排在主样式之前，保留层叠次序；管理员页独立 CSS 的导入不动。新增结构测试防止样式丢失和版本错配。前端版本清单已统一重生成为 `051b8b4aee223f30`，144 个 nativeFiles；91 个文件更新的是同一版本引用，不代表 91 处功能改写。
原计划 F2 已改为按最大 70px 展示的 2 倍缩放到 140×128（保留比例 / 透明背景）：15435 字节；原图保留在证据目录，实际页面验收未做。

目录规范与 Teacher 预留说明已补入项目（仅文档，不引入代码或调用）：[源码目录规范](source-layout-guidelines.md)、[Teacher 脚本边界](../../scripts/ai/teacher/README.md)、[Teacher 领域边界](../../backend/ai/teacher/README.md)。其它模块待办已登记；C++ 实际迁移本轮已执行，状态见下方阶段补充。

### 本地项目同步补充（2026-11-27）

- 实际源码工作副本是 `validation-runs/20261126-function37/source`，当前分支 `Function3.7`；本地源码、测试和文档同步不等于 Git 远端同步或服务器部署。
- 目录规范已登记至 `docs/catalog.json`。文档链接检查与 17 模块业务文档检查通过。
- 新增 `AiChatService::prepareAudit` / `prepareHistory` / `prepareRag` 三个固定监控项，共 29 项；Observer 查询自动沿用目录，不新增敏感标签。审计返回 false 会记录 error，历史和 RAG 未执行则不伪造样本。
- 新增 Observer 固定有界查询测试；C++ 聊天测试增加三段成功计数和审计拒绝不继续读取的断言。测试显式禁用 OTLP 与日志外发。
- 本机 MinGW 对准备阶段、公共遥测操作实现和 C++ 聊天测试的语法检查通过；完整遥测编译因本机没有 `openssl/rand.h` 未通过。未安装依赖、未执行 Linux 构建，也未运行新增 C++ 断言。
- 原 `token` 事件 / 审计 `outputTokens` 是文本片段数，不是 tokenizer token；未把它包装成 token/s。真实分词器指标本轮已实现并通过隔离合成契约；真实模型测量仍是待办。

- 完整源码检查 `optimization-preparation-source-ci-20261127-r6.json` 通过（40 组）；Windows Observer HTTP 项明确未运行。新增 Observer 查询 4 项、固定目录/缺样本契约 1 项通过。`git diff --check` 通过。
- r4 / r5 的本地失败证据保留：PowerShell stderr 处理中断及沙箱 `WinError 5`，不记为通过。经批准在沙箱外重跑的是相同范围离线源码检查；没有远端访问、安装或部署。

## 2026-11-27 持续执行：推理与目录阶段

### 已完成且已有证据

- AI 准备 / 审计 / 完成入库使用独立有界执行器，默认 workers=2、queue=32；仍共享原 PostgreSQL 连接池，不宣称消除了所有数据库争用。
- ONNX 使用实际 token ID 计数；首 token 包含 AppendTokenSequences 的预填充，加载 / 队列独立计时。gRPC trailing metadata 严格校验回传；旧 Worker 缺测不补片段数。
- 推理 / RPC executor 内容无关快照接入固定指标；Resilient 聚合主备且同指针去重，不持自身锁调用 Provider snapshot。
- 修复真实历史顺序错误：historyBefore(…, 0, 64) 已按升序，不再 reverse。新增 70 条历史的末尾窗口测试，当前问题保留为最后一条。
- C++ 四组契约在 Linux 隔离副本连续三轮通过：generation_metrics / ai_chat / ai_resilience / grpc_onnx_provider；主服务和 Worker 编译成功。该结果在目录迁移之前，迁移后扩大回归另列，不能混称同一版。
- Worker 日常生命周期为显式 `--ai-worker`，已有 TLS、token、loopback、taskset/nice、精确 PID/ticks、优雅停止和启动失败回滚；8 项 mock 契约 Windows / Linux 通过，未正式启用。
- 脚本阶段迁移 11 项，4 项目录契约 + 7 项导出合成契约通过；旧命令保留兼容。未下载 / 安装 / 训练。
- 经批准、备份和摘要核对后迁移 15 个 AI C++ 组件，更新 54 个相关文件；3 项目录契约与 frozen core 边界检查本地 / Linux 通过。代码链接重定向不改历史测量正文。
- 前端小研图标 714901→15435 字节，减少约 97.84%；按最大 70px 使用保留 2 倍采样。统一资源版本 `1f08ad808b467825`，144 nativeFiles，2 个二进制图片纳入版本，避免 immutable 缓存旧图。

### 本轮证据与未完成边界

证据目录 `validation-runs/20261126-function37/`：`optimization-linux-build-20261127-r2.log` 至 `r6.log`；r2 / r3 的失败保留。
r2 / r3 暴露并最终定位历史反转缺陷，r4 四组 CTest 三轮全过。r5 目录迁移和 22 项 Python 契约通过，边界检查因旧源码包遗漏 `.inc` 失败，r6 补生成并核对与本地 SHA 后，webserver / Worker / smoke 编译通过；迁移后的 generation_metrics、core、ai_chat、ai_resilience、rag、training、onnx_provider、grpc_onnx_provider 共 8 组连续三轮通过（24 次）。
完整离线 source CI 的 r7 / r8 属于先前阶段，不覆盖后续目录与图片变更；最终 r10（`optimization-layout-assets-source-ci-20261127-r10.json`）通过，45 项登记中 44 项通过，Windows Observer HTTP 明确未运行。

仍未完成：真实模型质量 / tokenizer token/s / TTFT / RSS 对比；新训练→ONNX→Adapter实产物；PG A/A / A/B / WAL / HOT；剩余 Envoy h2/溢出诊断；真实浏览器权限 / 控制台 / 缓存验收；正式默认模型 / Worker 切换、提交推送、旧目录与服务清理。
这些不能因为源码测试成功就标为完成；涉及正式服务、库写入、删除或发布仍需当前明确审批。

### 真实模型短测（r7，已执行）

同一隔离 VM、CPU 2/3、nice=5、最多 16 个输出 token，每模型独立进程 3 次，六次均成功。

| 模型 | 实际输出 token ID | decode token/s | Provider 首 token 秒 | 进程峰值 RSS KiB |
| --- | --- | --- | --- | --- |
| Qwen2.5-0.5B base | 7 / 11 / 7 | 47.1565 / 50.7846 / 48.4265 | 0.3660 / 0.2863 / 0.2537 | 567984 / 568176 / 568524 |
| Phi-3 | 10 / 10 / 10 | 8.0159 / 8.1550 / 7.7398 | 1.4904 / 1.3868 / 1.3401 | 3281868 / 3274324 / 3287604 |

证据：`optimization-real-model-smoke-20261127-r7.log`。smoke 的 validateVersion 预加载了模型，usage.modelLoadPerformed=false，不能把进程总墙钟当冷加载；短输出不是 512 token 测量，也不是质量达标。尚未据此切换默认模型。

新增独立合成评测工具正在本地验证：首个请求不调用 validateVersion，测进程冷加载（包含校验和验证，但不清空 OS 页缓存）；后续同进程请求测热路径。固定新版本分类 24 题 + 闲聊 3 题 + 知识 3 题，与历史数据库评测集明确区分。长输出按实际 token ID 报告，提前 EOS 不冒充 512 个输出；开放回答保留人工评审，不自动授权激活。

### 模型对比与数据库观测补充（进行中）

- 新离线 benchmark 的 C++ 契约在 Linux 连续三轮通过，Python 9 项通过；独立新 binary 已构建。前两次尝试分别因 CRLF/LF 原文件摘要不一致、未显式重配 CMake 而停止，未掩盖失败日志。
- 新增评测工具后的 source CI r13 通过：46 项登记中 45 项通过，Windows Observer HTTP 未请求。该结果不覆盖之后新增的 PG 聚合观测，后者单独复测。
- Qwen 合成质量 90 次回答已完成，严格分类为 61/72（84.72%），不是人工开放问答质量分；开放回答仍待逐条检查。进程首次加载 2.929324 秒（含摘要验证），后续 89 次首段文本中位 0.392362 秒，整个进程峰值 606160 KiB。
- 新增 PG WAL / IO / 指定业务表 HOT 聚合快照：查询前核对标记隔离集群；不读消息、用户记录或 SQL 文本，不重置统计。统计重置、计数回退、摘要异常会拒绝输出差分，零更新的 HOT 比率返回 null。六项新合成契约和既有 10 项 settings / 9 项 tuning gate 通过，尚未执行真实 A/A 或 A/B。
- 压测进程改成环境白名单，避免继承凭据、模型、代理和遥测变量；原共享线程池和耐久性不降低。尚未据此启用正式 PG 参数。

### 同条件模型对比结论（r13，四组均完成）

每模型质量 90 次 + 长输出 3 次，共 186 次生成成功。使用相同 binary / 两个固定题集、CPU 2/3、nice=5，串行执行；原始 binary SHA 为 `59bb2f1fd57af856165defce3ebf095be24b7c9f0c7bf35baee5dd74a9edbe2b`。

| 指标 | Qwen2.5-0.5B base | Phi-3 |
| --- | --- | --- |
| 严格单标签分类 | 61/72 (84.72%) | 0/72 (0%) |
| 首标签正确，仅诊断、非严格分 | 61/72 | 72/72 |
| 质量进程首次加载，含摘要验证 | 2.929324 秒 | 13.509477 秒 |
| 质量进程后续首段文本中位 | 0.392362 秒 | 2.419616 秒 |
| 长输出实际 token ID | 512 / 512 / 512 | 512 / 512 / 512 |
| 长输出 decode token/s 中位 | 41.741975 | 7.373064 |
| 长输出请求墙钟中位 | 12.915292 秒 | 76.605794 秒 |
| 长输出进程峰值 RSS | 612328 KiB | 3507740 KiB |

**不能据此切换默认模型。** Phi-3 严格分为零主要是标签后继续解释 / 编造后续题，不等于语义分类能力为零；不修改原始精确计分。首标签诊断下 Qwen / Phi-3 比为 84.72%，不足原定 90% 参考线；该诊断也不是原数据库评测集成绩。开放回答中，Qwen 在 TCP/UDP 与 Python 元组的事实正确性上有明确问题，Phi-3 多题输出冗长并被 128 token 上限截断。速度、内存优势已测到，开放质量 / 指令遵循门槛未通过，保持正式绑定不变。

证据：`optimization-benchmark-r13.log`、`optimization-pg-model-observation-r17.json`、`optimization-model-synthetic-review-r16.json`。首次加载是进程冷加载而非清空页缓存后的磁盘冷启动；这些结果尚未证明普通聊天在推理期间不受干扰。


## 2026-11-27 真实训练闭环与压测启动诊断

### 训练 / 导出 / C++ 推理：烟测通过，未激活

- r18 使用已批准的本地 Qwen 源模型生成 24 条合成数据，CPU 执行 2 步 LoRA 训练，耗时 27.19 秒。权重增量 L2 为 0.1649365029072374，证明参数发生了真实变化，不是空训练。
- 初次真实导出成功，但 C++ 生成失败并报告 `Empty chat template.`。根因是导出器支持文件白名单遗漏参考目录中的 `chat_template.jinja`；修复为逐字节复制该文件，新增对应合成契约断言，保留原失败记录。
- r22 复用同一训练权重重新导出，耗时 72.41 秒；参考基础 ONNX 图 SHA256 保持一致，模板摘要匹配，C++ 模型烟测通过（4.49 秒）。没有重新训练掩盖问题，也没有注册 / 激活候选模型。
- 证据：`optimization-training-r18.log`、`optimization-export-r22.log`、`optimization-export-pg-failure-r24.json`（位于工作区验证目录，不随发布包包含模型或认证文件）。这是管线可运行证据，不是微调质量提升证据。

### PostgreSQL：启动段错误已定位，尚无有效性能成绩

- 仅对固定 marker 校验通过的 socket-only 隔离 PG 18.6 集群运行，未连接正式数据库。r19 实际身份 / 参数 / 聚合观测成功；最新增加的 timing 字段与时间保护仍需实际工作负载前后复测。
- r23 / r27 A/A 在第一个测试服务就绪前退出。r27 补充返回码诊断后确认 `-11`（SIGSEGV）；没有有效 A/A 样本，未继续 A/B，也未改变全局 PG 参数。
- r29 使用已有 GDB、禁止 core dump，仅采集最多 24 层函数名和源码行，不输出参数或局部变量。调用链为 `SSL_CTX_new`（gRPC 的 BoringSSL）→ `TlsContext::create` → `ServerRuntime::start` → `ApplicationRuntime::run`。
- r30 链接检查确认同一可执行文件包含 `/usr/lib64/libssl.so` / `libcrypto.so`，并带入 gRPC 安装前缀中的静态 `libssl.a` / `libcrypto.a`。编译通过不代表 TLS 整程序启动可用；不能把本次崩溃归咎为数据库调参收益或噪声。
- 下一步：保持 `backend/server` 冻结，构建阶段拒绝混合 TLS 提供者；经明确批准后，复用固定源码在新隔离目录重建同版本 gRPC 的系统 OpenSSL 变体。TLS / gRPC 同进程烟测通过后才能重新校准，旧失败证据保持不变。
- 诊断证据：`optimization-pg-exit-r28.json`、`optimization-pg-gdb-r29.json`、`optimization-tls-link-r30.json`。这些诊断未修改正式服务、未读取凭据内容。


### TLS 构建保护及最新离线回归

- 已在 gRPC 包查找后调用 `cmake/GrpcTlsCompatibility.cmake`，发现包导出自带 `gRPC::SSL` / `gRPC::Crypto`（含小写兼容名称）时在配置阶段拒绝继续，提示用同版本、同 OpenSSL 的 `gRPC_SSL_PROVIDER=package` 重建。不提供关闭检查或跳过 TLS 验证的开关。
- 新增 4 项离线 CMake 测试通过（包含 4 种混用目标和 3 种缺失目标子用例）；临时目录访问被沙箱阻断的失败保留，经明确批准在沙箱外同范围复测通过。
- 最新 source CI r36：48 项登记、47 项通过，Windows Observer HTTP 未请求；覆盖聊天模板修复、新 PG 聚合观测、构建保护、权限前端契约及冻结边界。`git diff --check` 返回 0；既有 LF/CRLF 提示不是通过整程序启动的证明。
- 同版本 gRPC 的隔离 OpenSSL 变体构建已经逐次审批：使用已有固定源码与已安装依赖，全部 provider 设为 package，禁止 FetchContent 下载 / 更新，不覆盖全局安装。首轮 r35 被监测器与 CMake 临时文件重命名竞态中止，没有安装结果；r37 修复监测器后在新的隔离目录重试，仍在执行，不能提前宣称成功。
- 发布归档仅包含运行时文件和已验证二进制，CMake 保护属于源码构建输入，不必放入运行时文件白名单；正式发布和版本清理仍未执行。


### Envoy 只读协议 / 溢出诊断：有限样本通过，未改阈值

- r38 共 100 次健康 GET 全部 200：HTTP 网关 20 次冷连接 + 5 次同进程请求，HTTPS 自动协商 20 + 5 次、显式 HTTP1 20 次、显式 HTTP2 5 次，原生 TLS 20 + 5 次。HTTPS 自动协商与原生 TLS 实际返回 HTTP2；所有 HTTPS 均使用对应公开 CA 验证，不使用 `-k`。
- r38 的过滤统计查询没有匹配结果，r40 / r41 / r43 确认带 query 的统计接口 403。不能把空结果解释为零；没有扩大管理端权限或修改白名单。
- r44 只使用原本允许的精确 `/stats/prometheus`，在 VM 内限制响应大小、只保留指定聚合计数；另做 15 次健康请求，三条链路均观察到连接数 `1,0,0,0,0`，确认后四次复用已有连接。
- r44 前后选定 TLS 集群所有 overflow 指标均为 0，连接失败为 0；历史 `upstream_cx_destroy_remote_with_active_rq=1` 未增长。该窗口没有证据支持扩大熔断阈值，故保持不变；它不能解释或抹去早先页面的连接提前终止事件。
- 同时仍在编译依赖，所有本轮网关耗时仅作诊断，不作为优化收益或容量结论。没有访问真实用户数据、业务日志或登录凭据，也没有更改配置 / 重启服务。
- 证据：`optimization-gateway-protocol-r38.json`、`optimization-gateway-admin-diagnostic-r43.json`、`optimization-gateway-counters-reuse-r44.json`。最新 r39 source CI 仍为 48 项登记、47 通过、Windows Observer HTTP 未请求，并覆盖 CMake 辅助文件进入源码指纹的新增回归。
