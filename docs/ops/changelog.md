# 开发与运行变更记录

当前工作见[状态页](../../STATUS.md)。

## Function3.1 之后的本地开发

- `4b183f5` 把社区内容装配和事件投递装配拆成独立目标，明确发布、Broker 回调、语音和实时 Sink 的释放顺序。[模块说明](../architecture/business/engineering/community-runtime-modules.md)
- `62544ac` 加入 Linux 通信档和临时 PostgreSQL 业务档。两档均从空目录构建启动。业务档 112 项 CTest 通过，跳过 1 项可选 Redis 契约，真实发布、通知与重启恢复通过。[复现记录](validation/reproduce.md)

## Function3.1 及此前记录

最新页面更新（2026-11-06）：聊天、苦情树和账号页面共用水岸背景，登录和注册已同步更新。18090 资源 `366b63e5aaf26c0b`，就绪检查通过。见[版本说明](validation/function3.1.md)。以下保留此前补丁与 Phase49 历史验收。

11月5日背景补丁：镜像樱花水岸已获批准并上线18090，资源 `919e98a2ba67ff14`、PID27634。真实Linux隔离路由与浏览器解码复验通过；79项前端、23项工程检查通过，二进制/原库68账号密码散列/Token/证书/迁移24保持不变。首次直接图片路由404的回退记录保留。见[背景说明](../architecture/business/client-ui/sakura-lake-background.md)与[2026-11-05 背景原生资源验收](validation/function3.1.md#2026-11-05-背景原生资源)。

11月5日聊天补丁：三分钟撤回、ACK独立收敛和神树背景已上线18090，资源版本 `b72df43c75b2ce02`、PID22556；原库68账号及密码散列不变。79项前端通过；Linux相关Release四项及Debug/适用Sanitizer专项、真实隔离HTTP/WS/PG与三尺寸浏览器检查通过。见[说明](../architecture/business/client-ui/chat-recall-and-tree-background.md)与[2026-11-05 三分钟撤回验收](validation/function3.1.md#2026-11-05-三分钟撤回)。以下语音上线记录属于此前补丁，不代表当前运行版本；此前“程序不变”仅指当次语音静态更新，本次重新构建了撤回业务规则。

本轮免费语音替换：默认Kokoro＋浏览器中文朗读，已删除停用代理请求、地址表单与专用转码。已上线18090（资源9f3342acc65db2d7，PID17612），见[2026-11-05 免绑卡语音验收](validation/function3.1.md#2026-11-05-免绑卡语音)。74项前端、23项工程契约与41项独立发布包测试通过；原库68账号/密码散列、程序、Token密钥、证书与迁移24不变。未开通云账号、未安装依赖、未推送GitHub。

架构总览和 [完整调用图](../architecture/business/flows/current-end-to-end.md) 已按现有实现核对；[Phase50–53](../roadmap/next-steps.md)尚未验收为完成阶段。[工程可信度计划](../roadmap/engineering-credibility.md)已落实首批工具防护，完整A/A实测与性能修复仍待执行。

文档已按主题分类，入口见 [文档导航](../README.md)、[Phase时间线](../roadmap/stage-index.md)。Phase49历史验收结果保持不变；此快照保存在 `Function3.1` 分支，GitHub同步不代表部署日常服务。

Phase49完成本轮接口对齐、领域资源装配与数据库对象分层。**功能回归通过，严格性能门槛仍未通过。**

- 本地分支 `feature/function3-tree`；本轮前检查点 `42b3f8f`，Phase48与更早历史报告保留。
- 通信核心152份文件不变；main由964行/37个unique_ptr缩成14行进程入口，资源归各领域Runtime管理，显式幂等停止。
- 119项HTTP方法/路径、18个领域绑定；106处前端字面路径迁移。字段、权限、WS/SSE和Windows本地接口仍有各自契约，路由目录不冒充完整Schema。
- 账号、联系人、聊天、文件服务与前端控制器按领域归类；旧头文件与少数兼容委托保留。用户搜索独立归联系人目录。
- 数据库明确Query/Command、业务对象、驱动内部行、公开投影；复用原事务和连接，不新增数据库或迁移（仍024）。
- Linux三种矩阵各115项；前端33项、工程14项、七组浏览器、Windows两种构建各5项通过。10组真实业务、真实Windows代理、独立Worker故障与12项装配失败检查通过。
- 完整三轮：/fast中位-3.89%，混合吞吐中位-16.93%，错误0；ACK第3轮8.710→15.545ms（+78.47%）超标。中断的一轮单独保留，未采用较好记录替代。
- 聚合指标指向该轮SQL与投递同时变慢，没有队列满/连接池超时；逐消息COMMIT→ACK发送因果定位仍待补齐，不据此宣称重构提速。
- 未安装新依赖、未写原业务库、未替换日常服务；本次同步源码与文档不改变这些运行边界。云Key、语音自然度、远程CI与生产容量的历史限制保留。

入口：[改动与验收](../architecture/business/engineering/phase49.md)、[接口接入教学](../architecture/business/guides/phase49-teaching.md)、[数据库分层](../database/domain-layering.md)、[代码索引](../architecture/business/indexes/phase49-code-index.md)、[整体调用图](../architecture/business/flows/phase49-end-to-end.md)、[验证与回退](validation/phase49-validation-and-rollback.md)。

## 当时的后续计划

1. Phase50：先做逐消息耗时关联与性能修复，维持原三轮门槛，未通过不推进验收。
2. Phase51：真实多用户/多端闭环、字段/事件契约、稳定性和恢复演练。
3. Phase52：AI取消回收、模型资源/切换、知识授权、质量评估和语音试听。
4. Phase53：功能/性能门槛通过后再做限定容量的受控试运行与部署回退。

2026-11-04另有[语音与神树交互修复](../ops/runtime/speech-settings-and-playback.md)：个人与全站语音设置入口、有界 WAV 播放、管理员 HTTPS 私有凭据更新，以及边界滚动切页。新增接口后当前路由数为120；上文119项与115项等保持原Phase49验收口径。

修复测试复用隔离数据库，未增加迁移、依赖或新业务库；后续18090部署状态见下方11月5日上线记录。真实云Key、听感与正式容量不因本地试听通过而自动验收。

本轮[历史验收摘要](validation/function3.1.md#历史补丁)：44项前端、Windows Release 5项契约通过；Linux凭据与音频组件分别完成Release/Debug/适用ASan+UBSan检查，真实两音色与H2长音频分段验证通过，浏览器真实解码/手动续播和边界翻页通过。音频Sanitizer只覆盖测试/辅助实现及单独凭据组件，不冒充全服务器插桩；未重跑旧115项矩阵。

工程首批修正：显式A/A模式与二进制/配置/负载/夹具摘要，禁止label绕过门槛；不自动停服务或修改全机参数；拒绝缺失/重复轮次、不剔除极端值、不覆盖历史报告。新增9项离线契约通过，Linux预设解析与脚本语法通过；完整十轮校准、各预设编译、Docker与clang工具仍未验收。ObjectPool/BufferPoll已有真实引用，保持原位。

11月5日[历史验收摘要](validation/function3.1.md#历史补丁)：追加修复旧音色迟到播放和自动播放/暂停提示；7项新增用例使前端总数达到51项，工程仍23项。真实果实入榜→Outbox→男女声TTS→音频下载→单次领取、Windows个人语音与Range代理验证通过。当时发布包尚未激活，原报告保留。

11月5日[历史验收摘要](validation/function3.1.md#历史补丁)：用户明确批准后启动PID4328，程序`cb18d796…`、资源`511c4931f8cde9f2`。原库`phase11`的68个账号身份/密码散列、Token密钥、迁移024不变；HTTP健康与六个资源匹配，VM内部TLS校验通过，真实普通用户4.54秒本地试听到结尾。Windows直连18443仍超时，未改防火墙；生产滚动复测因电脑操作URL识别问题停止，不额外宣称通过。

后续[Azure音色目录补充](../ops/runtime/free-speech.md)：用户选择自有F0 Key，复用官方REST，新增有界目录查询与迟到试听保护；保留其他agent源码。57项前端、23项工程契约通过；从已验收页面包单独组装的24项语音/导航测试也通过。页面增量现已上线，PID7895、资源`c5a4f1e7f4934d1c`，二进制未换、原库无迁移；[2026-11-05 Azure 自选音色验收](validation/function3.1.md#2026-11-05-azure-自选音色)与[2026-11-05 Azure 自选音色验收](validation/function3.1.md#2026-11-05-azure-自选音色)已保存。其他agent的无关聊天改动仅留本地；云Key/真实试听仍待本人配置。

11月5日随后[历史验收摘要](validation/function3.1.md#历史补丁)：语音窗口固定操作栏、Key获取步骤、切换后清理旧试听；滚轮进度提示、约300ms切页、惯性保护与阅读位置恢复。65项前端、23项工程契约通过，独立发布包32项通过；浏览器验证含390×844窗口、试听播放和实际滚轮/键盘切页。当次日常PID9971、资源`5ec4d82e27389fab`，二进制不变，109份页面文件核对；68账号身份/密码散列与Token密钥未变，无迁移/安装/推送。真实Azure调用仍待自己的Key。
