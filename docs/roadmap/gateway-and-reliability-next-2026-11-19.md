# Envoy 网关与可靠性路线

2026-11-20 更新。双实例直连与真实监控链路已经验证。接下来先验收 Envoy 原生代理，再验收双上游切换、长稳和异机故障。C++ Proxy-Wasm 在原生代理稳定后开展独立对照。网关放在独立隔离入口，业务权限、持久化与恢复沿用 NEBULA 的边界。[下一阶段计划](next-steps.md) · [网关隔离启动与验收](../observability/envoy-gateway.md)

## 1. 能力放在哪里

| 层 | 负责 | 验收重点 |
|---|---|---|
| Envoy 原生配置 | 路由、TLS、WS 升级、超时、连接与请求上限、登录和注册入口限流 | 入口行为可以从配置、黑盒请求及原生指标复核 |
| C++ Wasm | WS 和 SSE 建连响应头计时，记录两种有界直方图 | 插件关闭、启用、故障时分别跑同一协议用例；比较 CPU、RSS、延迟与拒绝 |
| NEBULA 业务 | 鉴权、成员权限、幂等、持久化 ACK、撤回与恢复 | ACK 后强杀、请求重复、断线恢复和旧客户端兼容 |
| Observer | 查询入口状态、上游耗时、业务 Trace 与 PostgreSQL 分段 | 缺测可辨，失败能定位到网关、业务或数据库，样本可导出 |

Envoy 可以理解为统一入口的值班人员。它负责看请求要去哪里和入口是否已满。C++ Wasm 像插在这个流程里的计时器，读取有限的状态和将指标写回 Envoy。Envoy 官方标记 Wasm HTTP Filter 仍在实验阶段，因此首版把插件放在观测路径，故障时允许代理继续转发。插件方案依赖 V8 与 Proxy-Wasm ABI，扩展前需要单独验证构建和资源开销。[静态配置](https://www.envoyproxy.io/docs/envoy/latest/start/quick-start/configuration-static) · [熔断](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking.html) · [Wasm HTTP Filter](https://www.envoyproxy.io/docs/envoy/latest/configuration/http/http_filters/wasm_filter)

## 2. 如何推进

| 顺序 | 实施 | 退出条件 |
|---|---|---|
| A | 单上游 Envoy 隔离实验 | 原生配置校验和就绪通过。页面、鉴权、HTTP/1.1、HTTPS + HTTP/2、WS、SSE、文件与日志关联通过，记录一次上游超时和恢复 |
| B | 代理性能与入口追踪 | 同一业务二进制比较直连与原生代理。另行配置网关导出，验证关键请求的入口与业务 span 同 Trace，确认 Collector 中断时代理和业务可用 |
| C | 双上游健康摘除与回退 | A/B 均收到真实流量。A 退出后新连接到 B，在途已确认消息可恢复，同键只写一次。记录摘除、客户重连、恢复时长和回退结果 |
| D | 长稳、多机和原业务旅程 | 受限 24 小时运行，保存错误、P99、RSS、队列与磁盘。再用两台主机验证网络隔离、故障域和恢复。日常发布另有放行与回退演练 |
| E | 可选 C++ Wasm 建连计时 | 核验 SDK、编译器和 ABI。原生与开启插件时业务结果一致，采到两种握手直方图，插件出错时验证代理继续工作。三组配对观察资源与延迟 |

Envoy 已承载真实研途 HTTP 请求。A 的 TLS、长连接和故障脚本可以一键启动。C 已增加双地址配置，以及按业务 Trace 和持久化 ACK 验证健康摘除的脚本。E 已有原生 C++ 计时策略测试和插件源码。Collector、Tempo、Alertmanager 已在直连 A/B 中实测，原生入口 span 属于 B 的交付。[网关证据](../observability/validation.md#envoy-入口网关隔离验收) · [双实例证据](../observability/validation.md#双实例实时投递与告警验收)

## 3. 先跑懂双实例，再考虑流量治理

1. **先复用已验收的聊天路径**。同机两个进程对同一临时库启用迁移 025 和 `WEB_PHASE11_REALTIME_RELAY=1`。同一事务保存消息和 Outbox，各进程按投递游标向本地连接推送。旧 RocketMQ 聊天 Outbox 与这一模式互斥。适用 RocketMQ 的其他场景沿用各自的验收。[迁移与运行方式](../observability/dual-instance-lab.md)
2. **让两个上游都真正收到请求**。双实例网关使用静态双地址与主动 HTTP 就绪检查。故障验收在 A 持久化 ACK 后强杀，等待健康成员数变为 1，再核对请求 Trace、历史和相同幂等键重试。有计划的实例排空及重新接流单独演练。[Envoy 健康检查](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/health_checking) · [Envoy 连接排空](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/operations/draining)
3. **用客户端恢复验证业务**。WS/SSE 连接断开后，客户端按既有游标重连补发。结果不确定的聊天操作用原幂等键重试，网关保持写请求自动重试关闭。现有双端消息与历史已实测，跨实例还要补群聊、已读、撤回、会话吊销和慢客户端用例。
4. **把主机与数据库的故障分开演练**。第二台应用主机先验证断网、重连、实例标签和 Trace。随后另做数据库备份恢复，确定 RPO（可接受的数据丢失范围）和 RTO（可接受的恢复用时），再验证主备切换、连接恢复、旧主写入隔离及 ACK 边界。动态配置更新有实际规模和频率需求时，再验证 xDS 的更新、回退和控制面断连。[PostgreSQL 18 主备复制](https://www.postgresql.org/docs/18/warm-standby.html) · [Envoy 动态配置](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/operations/dynamic_configuration)

## 4. 性能定位与发布

1. 复用已有同版本 A/A 与长尾证据。实验条件变化时重新校准，再分阶段配对比较直连、Envoy 原生代理与启用插件。交替顺序，并固定业务二进制、夹具、并发和采样。保留 `/fast` 中位吞吐与每组 ACK P99 的原有门槛。[性能对照口径](next-steps.md#2-顺序与交付)
2. 分别记录客户端 ACK 往返、网关普通 HTTP 完成时间、上游响应头与业务 SQL 等待。WS/SSE 建连、首个业务事件和连接持续时长分别观察。单请求用请求 ID 或 Trace 对齐，每阶段分别报告 P99。
3. 从 Observer 的入口 5xx 和请求 ID 确认失败边界，沿业务调用查到数据库，再核对恢复后的历史和唯一写入。每轮保存开始条件、影响范围、恢复目标、实际用时与原始报告。
4. 在隔离库上保存已通过验证的入口配置和程序摘要，演练切回上一版流量路线，复查新连接、历史和幂等。日常部署单独验收和授权。监控中断期间观察导出队列、遥测丢弃及业务错误，恢复后用一条新 Trace 验证链路。[Collector 重试与缓冲](https://opentelemetry.io/docs/collector/resiliency/)
