# Function3 实施与验收台账

基线：Function2.1 `ffc0e14`。开发分支：`feature/function3-tree`。

## 现场核对（2026-10-23）

- 本地最初工作区干净。
- Linux `/home/pikachu/phase27-chat-reliability` 的聊天前端、历史模块、HTTP Session 和聊天 HTTP 适配器与基线逐文件 SHA-256 一致。
- `/home/pikachu/phase11-full-src` 的运行副本较旧，本轮不覆盖。
- Linux 约 7.5 GiB 内存、5.9 GiB 可用内存、7.4 GiB 剩余磁盘；已有 Java 24.0.2、RocketMQ 5.1.4、PostgreSQL、libsodium、curl、ONNX 环境。
- Three.js、C++ SDK、sherpa-onnx、Kokoro 与 FFmpeg 已逐项获准，程序均位于指定路径。复用现有 RocketMQ 5.1.4，没有下载 5.5.1。

## 阶段状态

| 阶段 | 状态 | 验证 |
|---|---|---|
| Phase28 模块/异步入口/前端/重连 | 功能验证通过 | Node 18/18，真实旧聊天黑盒通过 |
| Phase29 RocketMQ 与 Outbox | 真实链路与故障验证通过 | 5.1.4 FIFO 顺序通过，NORMAL TTS 真实消费通过 |
| Phase30 后台与权限 | 功能验证通过 | 越权、最后管理员、会话吊销、异步模型启用和失败状态均已验证 |
| Phase31 树业务与媒体 | 真实闭环通过 | PNG/WAV、坏媒体/超长语音拒绝、隐藏旧链接 404 |
| Phase32 3D 神树 | 分区与容量抽样通过 | Edge WebGL、移动、24 枚分区通过 |
| Phase33 榜单与语音公告 | 真实闭环通过 | 两种真实音频、公告播放领取去重通过 |
| Phase34 综合验收 | 存在未达标项 | Release/Debug/Sanitizer均107项通过；I/O对照已记录，ACK P99未达标 |
| Phase35 原库/语音/Redis/界面 | 本轮功能与回归通过 | Release/Debug/Sanitizer各108项；Node18项；原68账号保留；真实音频、移动三页、Redis回源通过；分类读中位吞吐+29.39%，不覆盖旧ACK门槛 |

Phase28–34历史验收见 [Phase34](phase34.md)；当前增量见 [Phase35](phase35.md)。上述“已实现”不等于生产认证。整体图见 [端到端架构](function3-end-to-end.md)，组件索引见 [代码索引](function3-code-index.md)。

## 文件与依赖规则

业务模块自行拥有领域、端口、应用服务与适配器；接口不暴露协议和数据库驱动类型。一个组件一个同名目录，配对源文件一起保存，纯接口不创建空实现文件。新增文件索引、调用图和测试证据随阶段同步。

业务数据与 Outbox 必须在同一事务提交，MQ 不替代客户端 ACK；媒体不扩大原聊天附件权限，树内容不自动进入训练数据。

## 当前日常使用入口（Phase35）

- 聊天：`/phase11/`；社区：`/phase11/tree/`；后台：`/phase11/admin/`。
- 三者复用账号 Cookie；普通用户不因成为群主而获得站点管理员权限。
- 首个管理员通过 `function3-site-admin` 显式指定现有普通账号；不会创建默认管理员密码。
- 默认公告声音关闭，用户主动开启并选择音色；播放声音不需要开启麦克风，只有发布录音时才申请录音权限。
- 部署先按顺序执行新增迁移，再配置已安装依赖；应用不会自动迁移或下载。
- 日常使用 `scripts/function3/runtime/service.py`，复用原 `phase11 / 5432`、原密钥和文件根目录，HTTP18090。升级前备份；本轮68个原账号摘要不变。首个管理员已按授权选择已有 `nebula_owner`，密码不变。
- `scripts/function3/validation/run_server.py` 只用于既有测试库与18094端口，不作为日常入口。本轮没有创建新数据库。
- 现有Redis6379用于公共分类/榜单读缓存；默认明确的本地持久投递支持自动公告语音。实际使用RocketMQ时必须显式配置可达Proxy。
- 统一顶部/移动底部导航，树/广场/榜单分视图，真实回复目标与举报弹层。操作见[日常启动说明](../../ops/function3-runtime.md)。

功能测试与性能门槛分别记录，性能失败不会被功能通过覆盖。

学习讲义：[一枚果实如何穿过服务器](function3-walkthrough.md)。本轮没有把性能未达标标为通过。

## Phase36–41 后续实施（2026-10-25）

- Phase36：聊天固定Grid区域、56+272/280+消息区、暗/浅主题、批量会话摘要与新backend边界已构建回归。
- Phase37：109项Release回归通过；统一动态/主页/收藏/可见性/管理删除和旧媒体权限真实HTTP闭环通过，神树与主页Edge检查通过。
- Phase38：Boost1.92已获准并核验，Windows系统SQLite3.51.1与DPAPI真实检查通过；两个助手入口的分离历史、共享记忆、中文词法检索和授权过滤通过。原生TLS/HTTP/WS/SSE代理、TXT/MD/PDF/DOCX解析、真实切号隔离与浏览器联调通过。
- Phase39：好友式/全局私人助手、群AI显式/提及/回复触发、限时私人推理、确认式白名单操作、旧资料复制校验与主动清除已实现；真实ONNX与迁移闭环通过。当前模型工具调用明确降级为问答/草稿，不声称自主Agent。
- Phase40：个人DPAPI语音设置、站点音色审计、Azure/Gemini固定适配、本地断句和语速已实现；Release111项、Windows5项契约及真实缺Key回退通过。真实云调用缺Key未验收，听感仍需人工判断。
- Phase41：Release/Debug/Sanitizer各111项、Windows两种构建各5项、Node18项及真实代理/生成/取消/Worker故障恢复通过。三轮 /fast中位回退6.56%合格，但两轮持久化ACK P99超标，性能门禁未通过；原始结果与WAL诊断见[Phase41](phase41.md)。本轮不切换日常服务、不推送远程。
- 18090仍运行此前版本；原业务库/账号未改。开发验证使用已有隔离库与18094/18444，不创建新的业务数据库。
- Phase34原持久化ACK P99未达标记录保留。

## Phase42：响应与外观优化（架构方向保持不变）

普通短请求复用已验证的TLS连接，神树批量读取与前十查询合并，静态树干合批、离屏暂停与果实资源增量更新；花穗、珠光果实、根部落花与窄屏构图细化。既有客户端/服务端/前端职责与数据库不变；测试与完整文件索引见[Phase42](phase42.md)。
