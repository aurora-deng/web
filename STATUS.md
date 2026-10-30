# 当前开发状态（2026-10-30）

Phase48继续补齐Phase43–47当前计划中的配色、领域仓储和前端事件依赖拆分。**功能回归通过，严格性能门槛仍未全部通过。**

- 分支`feature/function3-tree`；本轮变更前检查点`1c3c7e6`，更早基线`6dafe1e`保留。
- 通信核心152份文件不变；PostgresStore 2586→862行、InMemoryStore 1506→809行，SQL/状态操作归入对应领域驱动，事务连接及内存整体快照保留。
- Domain/Ports改为兼容聚合；HTTP入口拆成4个适配器，ChatBindings拆成8组事件绑定并声明各自依赖。
- 深浅主题、16份组件样式和动态弹窗统一语义色；删除叠加补丁Theme.css。三种尺寸及抽样文字对比度通过，树形与3D材质保留。
- Outbox成功前缀批量确认；新增提交通知/批次观测，修正后台线程先于计数器初始化的顺序风险。
- Linux Release/Debug/ASan-LSan-UBSan各115/115；前端28项、工程12项、七组浏览器、Windows两种构建各5项通过；真实业务、模型、Worker故障、语音回退及原生代理另有记录。
- 最终对最初Phase43基线：/fast中位+0.43%、混合吞吐中位+19.05%、错误0；ACK第3轮+23.51%超标。对直接上一版的最终测量也有两轮ACK超标，全部失败记录保留，不能采用前序较好测量替代。
- 远程CI、真实云Key、语音自然度及逐消息commit→wire关联仍有明确验证限制；不宣称生产就绪。
- 未下载依赖、未推送GitHub、未部署日常服务、未写原业务库；只复用现有隔离环境。

入口：[本轮改动与验收](docs/architecture/business/phase48.md)、[教学讲义](docs/architecture/business/phase48-teaching.md)、[组件索引](docs/architecture/business/phase48-code-index.md)、[验证与回退](docs/ops/phase48-validation-and-rollback.md)、[完整架构图](docs/architecture/business/phase43-47-end-to-end.md)。网页状态入口`/phase11/status/`。
