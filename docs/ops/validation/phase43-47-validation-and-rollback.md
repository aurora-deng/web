# Phase43–47验证、部署审批与回退

## 1. 本轮运行范围

- 本地源码：`github-test6.3-staging`，分支 `feature/function3-tree`；保留已有工作，无GitHub推送。
- Linux验证源码：`/home/pikachu/function3-src`，复用原有工具链。
- 隔离HTTP18094 / HTTPS18444；日常18090/18443没有自动部署。
- 原 `phase11/5432` 不用于测试；功能联调使用已有 `function3_runtime_test/55434`，契约/压测使用已有 `function3_phase11_test`、`function3_baseline_test`。后两者可重置，前者保留既有夹具；迁移只追加。
- 原始证据：`/home/pikachu/phase11-data/function3/validation/phase43-47`；本地整理到 `artifacts/function3/phase43-47`。不把连接密码、Key或本地资料放进报告。

## 2. 可复现入口

在项目根目录，使用已安装的Python、Node和工具链：

```powershell
python scripts/project/source_guard.py check
python scripts/project/ci_checks.py source
python scripts/client/build_windows.py --boost-root "E:\虚拟机高级web开发\deps\boost-1.92.0" --configuration release
```

Linux全业务矩阵必须显式指向已有隔离库。环境变量仅对当前命令生效，不写全局PATH；实际连接串从本机受控配置读取，文档不保存口令。

```bash
PHASE11_POSTGRES_CONNECTION='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test' \
FUNCTION3_REDIS_TEST_PORT=6379 \
python3 scripts/project/ci_checks.py full --build build-release

python3 scripts/function3/validation/build_checks.py debug \
  --evidence-root /home/pikachu/phase11-data/function3/validation/phase43-47/checks
python3 scripts/function3/validation/build_checks.py asan \
  --evidence-root /home/pikachu/phase11-data/function3/validation/phase43-47/checks
```

`build_checks.py`启动前检查磁盘预算；不足就停止，不删除模型或关闭保护。功能测试完成、停止隔离AI/TTS/服务并待CPU空闲后，运行 `phase47_benchmark.py`。它校验专用PID/端口、复用两个可重置测试库，交错执行三轮旧二进制/当前版本；不能把构建或推理同时跑在性能验收窗口。

## 3. 远程CI的边界

Workflow源码覆盖所有分支push/PR；全业务job需要经批准的 `webserver-isolated-ci` runner、`isolated-ci`环境，以及 `WEB_CI_POSTGRES_CONNECTION` Secret。Redis端口可通过 `WEB_CI_REDIS_PORT`设置。构建缓存必须属于runner当前checkout，并显式启用所需功能。

本轮不安装runner、不创建Secret、不启用云资源、不推送Workflow。远程运行结果因此不能填“已通过”。缺环境或功能时full入口直接失败；source成功不等价于后端成功。

## 4. 将来正式部署时

这一节是待批准的操作说明，本轮没有执行。

1. 记录日常版本、原库账号数量/摘要、迁移历史、配置与资源版本，生成并验证备份。
2. 在停止日常写入的窗口应用尚缺迁移，逐份核对；不另建用户库，不改旧迁移。
3. 使用同一套经验证的功能开关和原有端口，启动新二进制与配套前端。检查 `/health/version`、`/health/ready`、`/phase11/status/`、登录、消息ACK、权限和音频回退。
4. 若回退，先停止新进程；恢复经过确认的本轮前二进制及其前端快照。024为追加表/索引，可保留，不通过删表回退。原偏好数据保留，旧程序可能暂不显示新增会话免打扰。
5. 不回退到早于树故事/个人动态隐私修复的版本，否则可能扩大私有内容可见范围。

本地源码检查点 `6dafe1e`与公开源码归档可恢复变更前状态。恢复前先保存当前成果；不使用 `reset --hard`清掉工作，也不把数据库备份当源码归档替代品。

## 5. 停止与清理

仅停止脚本记录并校验PID起始时间的隔离进程和本轮Windows测试客户端/临时SSH转发。共享PostgreSQL、Redis、原库、模型目录和日常服务保留。清理只移除本轮确认无用的一次性编辑脚本；历史压缩包、源码、有效依赖和用途不明目录列入审计，不擅自删除。
