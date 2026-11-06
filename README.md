# Nebula Chat · C++ Web Server / Function3.1

一个以 Linux C++ 服务器为核心的学习与验证项目，包含聊天、苦情树社区、个人主页、三种 AI 助手入口，以及可选的 Windows 私人客户端。

**Function3.1** 基于 Phase49，收录三分钟撤回、免绑卡语音、神树滚动切页和登录到各工作区的统一水岸主题。本版是开发快照；正式容量和聊天 ACK 尾延迟仍待验收。详情见[版本说明与验证范围](docs/ops/validation/function3.1.md)。

[文档分类](docs/README.md) · [当前架构](docs/architecture/overview/ARCHITECTURE.md) · [完整调用图](docs/architecture/business/flows/current-end-to-end.md) · [当前状态](STATUS.md) · [后续计划](docs/roadmap/next-steps.md)

## 当前功能与边界

|区域|已有能力|需要区分的边界|
|---|---|---|
|账号与社交|注册/登录、Session/CSRF、好友、备注/免打扰/黑名单、在线状态|权限由服务器校验，隐藏按钮不代表授权|
|私聊与群聊|离线发送、持久化ACK、未读/送达/已读、撤回、群治理、历史/搜索定位、附件|ACK不是对方已读；重试保留原幂等身份|
|苦情树与个人动态|分类媒体动态、点赞回复、举报与隐藏、权限化主页、3D分区展示、七日榜/公告|个人可见范围与树公开范围分开，隐藏媒体旧链接也需鉴权|
|AI助手|好友式私人页、群AI、全局侧栏；Ollama/ONNX/独立gRPC Worker；连接测试适配|实际可用取决于配置；Windows当前不代表已部署本地大模型|
|RAG与模型治理|授权词法检索/引用、记忆、数据集/制品版本、评测门禁、训练转换及人工发布回滚工具|训练流水线验证不等于模型质量提高；不自动训练发布所有模型|
|语音与运行保障|独立TTS、公告与试听/回退、指标/健康、持久事件、Redis/MQ可选集成|真实云Key、主观自然度和正式容量有未验收项|

功能、替身、真实服务和性能分开记账。最新原始报告见 [Phase49](docs/architecture/business/engineering/phase49.md) 与 [证据目录](artifacts/function3/phase49/README.md)。

## 架构分工

```text
浏览器 frontend/phase11
  → 可直接访问Linux，或经Windows本机入口
  → 通信核心 backend/server（Reactor、HTTP/WS/SSE/H2/TLS）
  → 共享业务 backend/application（Service、Port、事务、DAO、Outbox）
  → AI核心 backend/ai（Provider、RAG、模型、独立重任务Worker）

Windows client
  → 本机代理、私人历史/记忆/词法索引、确认操作、凭据保护
  → 只把必要且获授权的片段与上下文发给推理服务
```

整体方向保持模块化单体加独立推理/语音/训练Worker。通信核心152份文件冻结，main只有14行入口；资源由各领域Runtime拥有。目录独立不是微服务，也不自动提速。[目录与依赖规则](docs/architecture/overview/project-layout.md)说明文件归属。

## 数据、接口与隐私

- Linux复用既有PostgreSQL业务库；当前迁移要求024，不随新功能另建公共账号库。
- 消息与Outbox同任务、同连接事务提交，之后分别形成持久化ACK和实时通知。
- 120项HTTP方法/路径、18个领域绑定不等于全量字段Schema；WS/SSE、Windows本地接口、外部模型调用仍有各自契约。
- Windows私人资料/索引在本机，授权片段可能上传推理；DPAPI保护凭据，未宣称正文整库加密或全部离线。
- 网页助手要求非保密上传提示；群AI不能读取个人私人知识。Key、模型、数据库、私人资料和本机账号文件不提交Git。

详见 [数据库分层](docs/database/domain-layering.md)、[接口契约](docs/contracts/README.md)、[接入讲义](docs/architecture/business/guides/phase49-teaching.md)。

## 启动、构建与检查入口

|需要做什么|入口|
|---|---|
|Linux日常启动/停止，原账号库与端口|[运行说明](docs/ops/runtime/function3-runtime.md)|
|Windows本机入口、Linux隔离验证|[本地预览说明](docs/ops/runtime/phase36-41-local-preview.md)|
|已有环境、依赖位置与审批|[依赖目录](docs/ops/dependencies/README.md)|
|测试矩阵、源码检查与故障验证|[测试入口](docs/testing/README.md)|
|部署前核对、回退与证据口径|[验证与回退](docs/ops/validation/phase49-validation-and-rollback.md)|

日常18090/18443与隔离18094/18444用途不同。源码分支更新不代表虚拟机日常实例已升级。安装、证书信任、原库写入与正式部署继续遵守既有审批规则，不自动下载依赖或修改全局环境。

文档检查可运行：

```text
python scripts/project/check_doc_links.py
python scripts/project/ci_checks.py source
```

新增公开文档后使用 `check_doc_links.py --refresh-catalog` 显式刷新目录；历史证据、私有账号文件和冻结核心不由文档改链工具重写。

## 验收状态

Phase49在2026-10-31归档：Linux Release/Debug/ASan-LSan-UBSan各115项，前端33项、工程14项、七组浏览器、10组真实业务、Windows构建/真实代理及Worker故障等有相应通过证据。2026-11-03文档分类又执行了源码与链接检查；这些日期与范围不能混成今天全部重跑。

**严格性能门槛仍未通过。** Phase49三轮/fast中位约-3.89%，混合吞吐中位约-16.93%；第三轮聊天ACK P99 8.710→15.545ms（+78.47%），业务错误0。原始不利结果与中断记录保留，不宣称生产就绪。

## 建议的下一步（尚未实施）

1. **Phase50：逐消息追踪与性能修复。** 先固定负载、评估VM的A/A噪声，再定位排队、SQL、COMMIT、ACK形成与客户端往返，一次只修一个有证据的瓶颈。
2. **Phase51：真实用户闭环与稳定性。** 补齐多端/断线/权限交叉场景、字段契约与恢复演练。
3. **Phase52：AI和语音治理验收。** 取消回收、驻留/切换、授权隔离、质量评估与真实试听。
4. **Phase53：受控试运行。** 通过功能与性能门槛后，再做容量预算、备份恢复、停机回退和部署审批。

完整目标、修改位置、测量边界与过关标准见 [后续路线图](docs/roadmap/next-steps.md)。暂不新增微服务、分布式数据库/缓存集群或继续堆模型。

## 学习与历史

- [按主题阅读](docs/README.md)、[Phase3–49时间线](docs/roadmap/stage-index.md)、[业务教学](docs/architecture/business/guides/README.md)。
- [当前代码索引](docs/architecture/business/indexes/phase49-code-index.md)、[历史通信详细讲义](docs/architecture/communication/runtime-baseline.md)。
- [版本差异](docs/releases/README.md)、[原始验收证据](docs/evidence/README.md)。较早README和当时未推送/未部署的叙述可在Git历史检查点`5f18761`查看。
