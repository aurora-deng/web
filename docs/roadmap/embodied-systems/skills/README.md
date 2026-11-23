# NEBULA Skill 合同 v1

`skillId` 和 `version` 一起标识一份合同。注册表按精确版本匹配。Agent 工具以认证过的协调器状态为证据，模型候选只通过结构化字段表达意图。当前版本提供只读查询与草稿能力。 [C++ 注册表](../../../../backend/agent/domain/SkillCatalog/SkillCatalog.h) · [Agent 学习与复验](../../../learning/nebula-fleet-agent-and-skills.md)

## 一、已接入母 Agent 的三个 Skill

| Skill | 输入与前置条件 | 输出与后置条件 | 权限、取消与失败 |
|---|---|---|---|
| `fleet.get_task_status@1` | 单个合法 `taskId`。协调器提供认证的 `Status`。工单 ID、阶段、`epoch`、Trace 和结果码相互一致。 | 最小证据快照，包含工单 ID、Trace ID、阶段、`epoch`、占用标志、待对账和结果码。只读。 | `--agent-sim` 下规划器使用独立只读凭据。临时不可用最多两次尝试，缺失与越权直接结束。每次读取共享该节点的期限，停止信号进入 gRPC。读取一批时全部核验成功才返回证据。 |
| `fleet.review_failures@1` | 1 至 3 个不同的工单 ID，已通过事实查询。至少一条是 `failed`、`uncertain` 或需要对账。 | 一份复盘草稿，引用相关工单。每个引用能在同次读取的证据中定位。 | 引用集合和顺序由本轮证据核对，无可复盘事实返回 `no_failure_evidence`。未知技能、错版本和不符证据的候选返回 `proposal_rejected`。 |
| `fleet.propose_sim_reschedule@1` | 单个现为 `queued`、没有分配机器人、无需对账的原工单。 | 一份模拟重排草稿，锚定原工单。`stage` 使用起草凭据将草稿保存为待审批。 | 草稿层仅允许这一固定模板。失去前置条件返回 `task_not_eligible`。审批角色核对同一条记录，协调器确认前重验 `epoch` 与阶段。 |

提案形状固定为 `skillId`、`version`、`evidenceTaskIds`。读取工具使用 600 ms 节点时限、每次图运行最多两次读尝试。规划节点预算 500 ms，其中 Provider 使用至多 200 ms。暂时不可用或超出 Provider 期限时，节点仍有预算就可使用规则；不符规则的候选进入拒绝。`--events` 输出的观察事件只含节点、状态、Trace 和固定错误码。 [策略实现](../../../../backend/agent/application/FleetAgent/FleetAgent.cpp)

## 二、机器人侧三个首批合同

现有边缘代码提供导航端口、仿真、ROS Action 和工单账本。下表给出子 Agent 的 v1 约定。准备度通过 `ready()` 核对，导航通过 `navigate()` 调用，结果由边缘账本持久化后经 gRPC 确认。三份合同共享边缘工单图。边缘握手声明已安装的 v1 能力，导航命令携带 Skill 版本，旧版省略附加字段时按 v1 处理。持有同一个导航端口的任务共享底盘互斥。 [能力注册表](../../../../backend/fleet/domain/RobotSkills/RobotSkills.cpp) · [ROS 适配](../../../../robotics/nav2_gateway/Nav2Navigation/Nav2Navigation.h) · [边缘工单图](../../../../backend/fleet/application/TaskWorkflow/TaskWorkflow.cpp)

| Skill | effect 与输入 | 开始条件与成功证据 | 失败、超时与对账 |
|---|---|---|---|
| `robot.get_status@1` | `read`。当前边缘身份和可选任务 ID。 | 身份绑定当前边缘；返回就绪状态、可用能力、当前工单引用和采样时刻。 | 离线与过期快照分别返回。短暂错误可重查，取消后停止等待。重复查询可观察到更新状态。 |
| `robot.navigate_to_sim_station@1` | `external`。批准的工单、`epoch`、登记过的模拟站点和 deadline。独占底盘执行资源。 | 核对能力、当前工单代号和本地运行许可；仿真 Action 返回到站，结果关联同一工单。 | 去重键使用工单 ID 和 `epoch`。目标拒绝与结果不明分开，断线或进程中断后先对账。取消请求和取消成功分别记录。Simulation 与 ROS 适配复用同一合同。 |
| `robot.report_task_outcome@1` | `idempotent`。`taskId`、`epoch`、终态和有限结果码。 | 已接收工单、报告来自所属边缘，允许的状态转移；落账后回显已保存的结果。 | 相同内容同键重放返回已有记录。旧代号或冲突内容进入拒绝。连接中断时按原编号重试或查询已落账状态。 |

三份草案的共同验收是正常、前置不满足、超时、取消和重复报文。导航再验共享底盘资源互斥、未知结果与重连。结果上报再验确认丢失、旧 `epoch` 和同键冲突。每次尝试能按 Trace 与工单 ID 追到测试夹具生成的记录。现场审批与硬件运行另走 M8/M12。 [M02 验收矩阵](../m02-skill-library.md) · [M08 ROS 边界](../m08-ros-robot-executive.md) · [M12 安全](../m12-safety.md)
