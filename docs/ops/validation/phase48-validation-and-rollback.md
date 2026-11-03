# Phase48 验证与回退

本轮只改源码与现有隔离环境。原账号库、18090/18443日常实例未升级；迁移仍为024。性能门槛未通过，不建议将这份验收当成生产发布许可。

## 复验入口

在项目根目录使用已安装工具。以下不会安装依赖；Windows源码检查要求现有Node可见。

```text
python scripts/project/ci_checks.py source
python scripts/project/source_guard.py check
```

Linux完整业务矩阵必须配置PG/Sodium和真实隔离数据库，缺少必需套件、GTest、编译驱动或功能开关会失败，不能绿色跳过。连接参数仅使用专门测试库。

```bash
PHASE11_POSTGRES_CONNECTION='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test' FUNCTION3_REDIS_TEST_PORT=6379 python3 scripts/project/ci_checks.py full --build build-release
python3 scripts/function3/validation/build_checks.py debug
python3 scripts/function3/validation/build_checks.py asan
```

Sanitizer打开泄漏检查及出错即停；有磁盘空间保护，不删除模型来绕过。Windows客户端继续用`scripts/client/build_windows.py`的release/debug配置及既有Boost；真实代理验证另需已确认的学习证书指纹和隔离服务。

实际回归、性能、截图与摘要位于`artifacts/function3/phase48/`。源码检查、替身浏览器、真实Linux、真实Windows分别记账；九组真实业务先于最后观测修正，最后修正后重跑了115项三种矩阵、工作区黑盒和指标冒烟。真实云Key及语音自然度没有以替身替代。

## 运行版本核对

最终构建后用`/health/version`的`sourceFingerprint`核对`scripts/project/build_stamp.py`计算值；同时检查`requiredMigration=24`、`/phase11/asset-manifest.json`及状态页verification.js。运行健康不等于性能验收通过。

本轮测速构建与最终状态页归档构建的摘要分别保存。不能把最终摘要倒填到之前测量。`benchmarks-final`与`benchmarks-previous-final`保留最终失败；未加final的前序成绩只代表当时构建。`diagnostic-on-final-durable`是诊断，不参与门槛。

## 回退边界

1. 本轮前检查点为`1c3c7e6`。先保存自己的新增内容，再从该检查点建立独立目录/工作树构建，不在有未保存内容的目录使用强制重置或清理。
2. 无新表或迁移，无需数据库降级。不得拿可重置测试库覆盖原库，也不得将本轮测试账号导入原库。
3. 只停止验证启动器拥有、且PID/启动标识/可执行路径一致的进程。原PG、Redis与日常服务不属于本轮停机集合。
4. `source_snapshot.py`与`unpack_snapshot.py`只处理公开清单；清理仅限上次同步拥有、内容未被改动的退役源码。未知文件、用户修改、数据、模型、依赖和构建产物保留。
5. 前端回退应连同原asset-manifest和全部资源引用一起回退，不能只换一个HTML；Theme.css已退役，不能继续挂载旧补丁。

后续性能定位先做固定到达率对照、逐次事务/刷盘/ACK关联以及宿主机同期资源记录。当前只证明存在同步WAL等待和批量确认降低提交开销，不把目录拆分解释为尾延迟保证。
