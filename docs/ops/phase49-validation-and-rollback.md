# Phase49 验证与回退

本轮只修改本地源码和已有隔离验证环境。原业务库、18090/18443 日常服务未升级；无数据库迁移。基于 `42b3f8f`，不自动推送 GitHub。

## 1. 可复验入口

```text
python scripts/project/ci_checks.py source
python scripts/project/sync_api_contracts.py --check
python scripts/project/source_guard.py check
```

source 检查覆盖冻结的 152 份通信文件、领域依赖、main 边界、静态资源版本、语义配色、路由绑定及导入、工程负向测试与前端逻辑测试。

Linux 使用原有隔离 PostgreSQL 集群的 `function3_phase11_test`，不使用日常库。先确认测试库可连接，再运行：

```bash
PHASE11_POSTGRES_CONNECTION='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test' FUNCTION3_REDIS_TEST_PORT=6379 python3 scripts/project/ci_checks.py full --build build-release
python3 scripts/function3/validation/build_checks.py debug --evidence-root /home/pikachu/phase11-data/function3/validation/phase49/checks
python3 scripts/function3/validation/build_checks.py asan --evidence-root /home/pikachu/phase11-data/function3/validation/phase49/checks
python3 tests/function3/integration/assembly_lifecycle_blackbox.py --server build-release/webserver --output /home/pikachu/phase11-data/function3/validation/phase49/lifecycle-release/summary.json
```

生命周期用例使用已存在的隔离数据库和私有测试 Key；缺 Key、TLS 文件无效、自己占用的 HTTP/gRPC 端口导致失败后，要求程序正常返回 1、不挂起、无 Sanitizer 错误。Debug 和 ASan 二进制分别复验。

Windows 使用已有 `scripts/client/build_windows.py` 与已批准的 Boost。此检出目录有中文路径，旧 `build-client/release` 是脚本产物目录，不能误认为 CMake 构建树；不为此另装工具链。

## 2. 证据口径

- `artifacts/function3/phase49/` 保存经筛选的公开报告、失败记录、原始日志、截图及摘要；用户资料、Key、数据库、模型和构建二进制不纳入。
- 浏览器协议替身验证真实页面与交互，不替代真实后端权限测试。
- 真实接口测试复用 `function3_runtime_test`，创建独立测试账号/内容；不重置这个库，不写原账号库。
- 基准只允许重置专用 `function3_phase11_test` / `function3_baseline_test`。保存实际旧二进制进行三轮交错对照，不用改完后的程序假装旧基线。
- 目录调整不是性能证据。Phase48 失败记录保留；新结果单独报告。
- 云模型/语音缺真实 Key、主观自然度、远程 CI 与生产容量等限制不能通过替身改成已验收。

## 3. 回退

1. 日常服务没有被替换，无须为本轮回退日常服务或原业务数据库。
2. 隔离实例由 `run_server.py stop` 按 PID、启动时刻与二进制身份停止；Worker 使用 `onnx_worker.py stop`，不按端口或名称批量杀进程。
3. 如需回看代码，在单独副本检出 `42b3f8f`；先保存当前未提交成果，不在现有脏目录强制 reset。
4. 不回退、删除或新建原数据库。本轮没有 schema 变化；隔离测试的账号/内容属于测试夹具。
5. 前端模块移动后，旧静态副本不能与新 HTML 混用。用资源清单和 `/health/version` 指纹确认运行的是同一份源码，缓存问题不能靠反复随机添加参数长期解决。
