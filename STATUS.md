# 当前开发状态（2026-10-29）

Phase43–47业务改造与本地验收记录已落地。**功能回归通过，性能门槛未全部通过。** 日常服务与原业务库没有随源码自动升级。

- 分支 `feature/function3-tree`；变更前可恢复检查点 `6dafe1e`，归档与原始摘要保留。
- 通信核心152份文件保持不变；新增用户搜索、会话免打扰、分页搜索/有界定位、上传能力契约，修正未读和偏好乱序收敛。
- 消息读取委托领域驱动，20条带附件历史页为两次驱动查询；聊天入口拆成9个控制器，共享一个上下文。
- Linux Release/Debug/ASan-LSan-UBSan分别114/114；前端24项、工程8项、七组浏览器以及Windows两种构建各5项契约通过。真实服务、模型、群AI、语音回退和代理另有证据。
- `/fast`中位吞吐约+0.23%，混合业务吞吐约+16.84%，错误0；第2轮ACK P99为+23.33%，超过20%，总体性能门槛保留失败。
- 远程CI实跑、真实云Key及精确逐事件commit到wire观测尚未验收。不据此宣称生产就绪。
- 本轮未下载依赖、未推送GitHub、未部署日常服务、未写原业务库。

入口：[验收与原始数据](docs/architecture/business/phase47.md)、[教学讲义](docs/architecture/business/phase43-47-teaching.md)、[完整架构图](docs/architecture/business/phase43-47-end-to-end.md)、[组件索引](docs/architecture/business/phase43-47-code-index.md)、[回退说明](docs/ops/phase43-47-validation-and-rollback.md)。网页入口为 `/phase11/status/`。
