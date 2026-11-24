# v0.4 Fleet 复现与验收

本轮暂停机器人硬件演进，回到 Fleet、DAG、Agent 与发布工程。Linux 上的验证使用临时 PostgreSQL、回环 Redis 和私有 SQLite。M09 的双仿真体压力和 100 Hz 时序结论沿用原测量。 [M09 实验](m09-actuator-control.md)

## 在干净检出复跑

准备 Linux x86-64 Docker、现有 C++ 和 PostgreSQL 等工具。以下是 CentOS 已有的私有安装布局。路径可按自己的安装替换。脚本只从当前检出和已安装工具构建，校验依赖制品摘要。设置 `--expect-lock` 可要求既有依赖与报告吻合。

```bash
dep="$HOME/phase11-deps"
args=(
  --output ../v04-reproduction
  --postgres-prefix "$dep/postgresql/18.6"
  --grpc-prefix "$HOME/.local"
  --sodium-prefix "$dep/libsodium/1.0.22"
  --curl-root "$dep/curl/8.22.0"
  --gtest-dir "$HOME/.local/lib64/cmake/GTest"
  --openssl-include /usr/include
  --openssl-lib-dir /usr/lib64
)
python3 -B scripts/project/reproduce_fleet_release.py "${args[@]}"
```

完整演练可追加 `--test-redis-port PORT`。仅使用为这次运行启动的回环 Redis。输出目录必须是新目录。

1. 构建 C++ 车队、Ollama 适配器与 PostgreSQL DAG Worker。精确核对 CTest 注册和执行结果。
2. 从空目录构建 Web 程序。对临时 PostgreSQL 应用连续迁移，检查注册、登录、WebSocket、SSE、幂等和重启恢复。
3. 打包并核验 ZIP。使用 `scratch` 与 `--pull=false --network=none` 构建 Web/Fleet 镜像。运行时通过只读挂载复用匹配的主机动态库。
4. 在容器内跑 Web、车队派发与回执、Agent 工具与检查点，以及 PostgreSQL DAG 汇合。导出并重新加载镜像，对比 Docker 镜像 ID。测试用归档放在临时目录。

同一台主机复跑时，把上次生成的锁提供给 `--expect-lock /absolute/path/dependency-lock.json`。锁按版本与制品内容判断，不绑定本地路径。Docker 报告单独保存了镜像 ID、压缩包摘要和只读挂载所需动态库的摘要。 [本次容器报告](../../../artifacts/v0.4-fleet/docker-delivery.json)

## 单独复验跨 Worker 恢复

```bash
ctest --test-dir ../v04-reproduction/fleet --output-on-failure -R nebula_workflow_pg_leases
```

夹具创建 `A → B/C → D`。先暂停已领取 A 的进程，另一进程等租约过期后接手。原进程恢复后提交迟到结果，确认被拒绝。外部副作用版本只执行一次尝试，进入待对账并跳过依赖。恢复证据以共享数据库的 UTC 时间为准，导入 Observer 后按 traceId 验证。

要独立试用队列，可在自己的测试库应用 [租约表定义](../../../backend/systems/PostgresWorkflowQueue/schema.sql)，配置 `NEBULA_WORKFLOW_DSN` 后运行 `nebula_workflow_worker submit demo read`，在不同终端运行 `nebula_workflow_worker work demo read worker-a 1500` 和 `nebula_workflow_worker work demo read worker-b 1500`。当前样例按固定租约预算工作，长任务心跳续租和多机时钟验收放在下一轮。

## 单独复验 Fleet Agent

在已有模拟协调器和只读凭据的终端执行。草稿和人工审批分别使用已有的角色凭据。

```bash
agent=../v04-reproduction/fleet/agent/nebula_fleet_agent
"$agent" tools
"$agent" "$FLEET_ADDRESS" list 0 8
"$agent" "$FLEET_ADDRESS" evidence "$TASK_ID"
"$agent" --journal ./operator-workflow.sqlite --session learning "$FLEET_ADDRESS" review "$TASK_ID"
"$agent" --journal ./operator-workflow.sqlite --session learning "$FLEET_ADDRESS" review
```

传入 `--ollama INSTALLED_MODEL` 使用现有回环 Ollama。默认地址 `http://127.0.0.1:11434`，也可用 `NEBULA_AGENT_OLLAMA_URL` 配置另一个本机端口。使用同一个 `--run-id` 和同一输入恢复检查点。若提案处于待对账状态，审批角色通过 `reconcile PROPOSAL_ID` 查询权威状态并完成本地对账。

先用 Ollama 的 `/api/generate` 加载已安装模型。请求体使用模型名与 `keep_alive`，省略 `prompt`。等加载完成后调用 Fleet Agent。查看结果中的 `planner`，`provider` 表示模型候选通过，`rules_fallback` 表示按证据使用规则。测试冷启动和预加载时各用新的 `run-id`，避免读到缓存的提案。 [Ollama Generate API](https://docs.ollama.com/api/generate) · [实测时序](../../releases/v0.4-fleet.md#2026-11-24-验收)

## 运维与后续验收

- 027 是追加迁移。部署到日常库前按本项目的备份与停机窗口执行。旧回执退休后，Outbox 行承担去重标记。将来设计 Outbox 清理时，两张表的保留协议要一起调整。 [数据库说明](../../database/README.md)
- PostgreSQL 队列的适配器按受信的已注册图执行，模型只提供候选参数。后续先补长期任务续租、跨主机容量基线和队列保留治理，再扩展外部写入技能。
- Fleet Agent 使用工单上下文，网页版个人助手继续走原会话体验。要统一私聊入口时，先确定用户授权范围、保留期限和上下文的跨端边界。 [Agent 与 DAG 学习](../../learning/v04-fleet-agent-dag.md)
