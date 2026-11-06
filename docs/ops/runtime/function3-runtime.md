# Function3 日常启动：原账号库、18090 与语音

> 本页说明日常实例。最近一次记录在 [Function3.1 发布说明](../validation/function3.1.md)，当前业务二进制要求迁移 001–024。本地社区拆分分支通过[独立复现](../validation/reproduce.md)验证。日常版本和测试范围见[当前状态](../../../STATUS.md)。

本入口用于当前 Linux 学习环境，复用已经安装的依赖。代码位于 `/home/pikachu/function3-src`；不下载软件、不新建数据库，不改变 Redis、数据库监听地址或防火墙。

## 1. 正常启动与停止

```bash
cd /home/pikachu/function3-src

# 首次升级时执行：先备份原库，再补齐缺失迁移。先停止本入口的服务。
python3 scripts/function3/runtime/service.py prepare

# 日常启动：原账号 + AI + 本地男女语音 + 现有 Redis 6379
python3 scripts/function3/runtime/service.py start

# 查看进程身份、数据库和日志位置
python3 scripts/function3/runtime/service.py status

# 停止本入口拥有的服务器；保留数据库、Redis、账号和上传文件
python3 scripts/function3/runtime/service.py stop
```

已经迁移完成后，日常使用只需 `start`。应用启动不自动执行 DDL；如果源码增加了迁移，先 `stop`，再 `prepare`、`start`。脚本会核验 PID、进程启动世代与程序路径，拒绝操作其他服务；不要用旧实例的停止脚本控制这个实例。

| 页面 | 地址 |
|---|---|
| 聊天、联系人 | <http://192.168.239.135:18090/phase11/> |
| 苦情树 | <http://192.168.239.135:18090/phase11/tree/> |
| 管理中心 | <http://192.168.239.135:18090/phase11/admin/> |
| 存活检查 | <http://192.168.239.135:18090/health/live> |
| HTTPS 示例 | <https://192.168.239.135:18443/phase11/> |

三个业务页面都有统一导航。HTTP 地址可用于文字聊天、上传已有音频与播放公告；浏览器录音需要可信 HTTPS 或浏览器认可的安全上下文。脚本生成的是学习证书，不自动把它加入 Windows 信任库；证书信任与麦克风授权仍由你控制。

`/` 不是必须的前端入口，直接使用上面的 `/phase11/`。若端口占用，先确认占用进程；脚本不会为释放端口杀掉其他程序。Linux 本机健康但 Windows 超时，应检查虚拟机网络和现有防火墙规则；脚本不会偷偷开放新端口。

## 2. 使用的是哪一套数据

| 项目 | 当前日常值 |
|---|---|
| PostgreSQL 数据库 | `phase11`，原业务库 |
| PostgreSQL 端口 / Unix socket | `5432` / `/home/pikachu/phase11-data/postgresql/run` |
| 原文件根目录 | `/home/pikachu/phase11-data/files` |
| 原 Token Hash 密钥 | `/home/pikachu/phase11-data/secrets/phase11-token-hash.key` |
| Function3 媒体派生目录 | `/home/pikachu/phase11-data/function3/media` |
| 升级前备份 | `/home/pikachu/phase11-data/function3/runtime/backups` |
| 迁移校验记录 | `/home/pikachu/phase11-data/function3/runtime/migration-report.json` |
| 日常服务器日志 | `/home/pikachu/phase11-data/function3/runtime/webserver.log` |

`prepare` 使用 `pg_dump -Fc` 备份并检查可读取性，随后只执行缺失迁移；比较升级前后的“用户 ID、用户名、密码散列”整体摘要。此次升级保留了 **68 个原账号**。新增功能在原库添加 `tree_*`、`function3_*` 等具名表，不另建账号库。

根据你允许任选账号的答复，已通过项目自带管理员工具将已有 **`nebula_owner`（林川）** 设为首个站点管理员。仍使用它原来的密码，没有创建默认管理员密码。授权操作保留审计，并吊销了该账号当时的旧登录会话，需要重新登录一次。普通群主不自动获得站点管理权限。

Navicat 继续连接原数据库的 `5432 / phase11`；主机、数据库用户名和密码沿用你的原配置。不要误连验证用的 `55434 / function3_runtime_test`。

## 3. 公告没有声音时如何确认

1. 登录后展开顶部“公告声音”，点击 **“试听当前音色”**。这是固定文案试听，不发布社区内容，也不要求麦克风权限。
2. 等待状态变为可播放，查看原生播放器。浏览器禁止自动播放时，点击继续播放或播放器播放按钮。
3. 开启公告声音，选择女声 `zf_001` 或男声 `zm_009`。开关仅控制新公告自动播放；不会把所有历史公告重新播一遍。
4. 前十名成员或名次发生变化时才有榜单公告；仅热度数值变化不播报。生成有延迟，文字先到，音频准备好后补充。
5. `GET /api/me/speech/status` 可确认引擎是否启用；没有配置引擎时界面会明确提示。音频失败、自动播放受限会有提示，不再只显示文字让人等待。

播放声音不等于开启麦克风。录制自己的语音才需要麦克风授权。耳机/系统音量、浏览器标签页静音仍由设备控制；自动化验证能证明 WAV 非静音、浏览器解码且播放时间前进，不能替代物理扬声器听音检查。

## 4. Redis 与消息队列

日常入口复用已经运行的 **Redis 6.2.19 / 6379**，缓存公共分类和榜单读结果。账号、权限、消息写入、ACK 和用户正文不放进这层缓存。Redis 忙或不可达时回落 PostgreSQL；停止 Web 服务不会停止共享 Redis。

默认事件投递为明确的 **本地持久投递模式**：PostgreSQL Outbox → 本地适配器 → Inbox → 语音任务。它允许单进程学习环境不启动 Broker 也能完整生成公告音频，不能据此声称 RocketMQ 正在运行。

已经准备好可达的 RocketMQ Proxy 时，可以显式使用：

```bash
python3 scripts/function3/runtime/service.py start --mq-endpoint 127.0.0.1:8081
```

这个参数只配置客户端，不负责启动 Broker。原有隔离 Broker 可能位于独立网络命名空间，不能直接把其内部地址填给外部服务。磁盘保护触发时也不能靠提高阈值绕过；请先处理明确可清理的占用或扩容。

按需关闭可选功能：`--no-ai`、`--no-speech`、`--no-redis`。这些参数只影响本次服务，不卸载依赖。

## 5. 测试入口与日常入口要分清

`scripts/function3/validation/run_server.py` 和 `isolated_postgres.py` **仅用于已有 `_test` 库的隔离验证**，仍保留 18094 等测试端口。原先按 `run_server.py start --ai` 启动时，既不是原账号库，也没有自动开启 TTS，这正是前面账号不一致和语音无输出的重要原因。

从完整源码检出开始做干净构建时，使用 `scripts/project/reproduce.py`。它会选择临时 HTTP 端口，业务档会初始化一次性 PostgreSQL，并在结束后清理自身资源。[复现指南](../validation/reproduce.md)

日常用本页的 `runtime/service.py`；测试继续使用已有测试库，避免将合成账号、坏媒体、举报隐藏夹具写入你的真实业务数据。此次没有新建数据库。

实现与验证详见 [Phase35](../../architecture/business/client-ui/phase35.md)。
