# Phase44：可执行的工程检查

## 1. 配置入口

[RuntimeConfig](../../../backend/application/config/RuntimeConfig/RuntimeConfig.h) 集中处理既有环境变量的布尔、整数、列表和跨字段检查；main中的辅助函数委托给它。`.env.example`只是说明，不自动加载，也不含真实口令。

校验包括：端口/队列范围、非法布尔、证书与私钥配对、业务库与Token密钥配对、Function3需要数据库；生产模式需要32字节以上认证密钥、明确Origin、TLS和持久库。各具体Provider仍保留自己的模型/路径校验，没有把全部驱动配置塞进公共Config。

有库配置时，服务在装配后台线程之前只读检查001–024迁移记录。缺失任何一份都拒绝启动，**不由启动过程自动执行迁移**。`/health/live`只证明进程活着，`/health/ready`继续检查运行依赖。

## 2. 四类测试与失败门禁

`scripts/project/ci_checks.py source`运行：通信冻结/领域依赖检查、资源版本检查、Python语法、原生JS语法、前端逻辑测试，以及CI失败门禁的反向测试。

`ci_checks.py full --build build-release`要求已配置的Linux构建、Phase11/PostgreSQL/libsodium/Testing均开启、真实隔离连接串和真正编译的驱动源文件。检查 `compile_commands.json` 中的 `PostgresStore.cpp`、`SodiumCredentialCodec.cpp` 及其定义；不能用另一个源文件里出现同名宏代替驱动证据。报告列出实际发现的通信测试、无PG业务测试、真实PG契约，以及可选AI功能开关。关闭功能、缺连接串、缺真实驱动或测试失败都会返回失败。

配置所用环境变量仍为 `PHASE11_POSTGRES_CONNECTION`，Redis契约使用已有 `FUNCTION3_REDIS_TEST_PORT`。新工具入口不安装任何依赖。Debug/ASan矩阵由既有 `build_checks.py` 在相同功能配置上构建。

`.github/workflows/ci.yml`已覆盖开发分支push和PR的源码检查。真实业务job指定专用隔离runner，不把不可信PR送进日常VM。本轮没有推送或配置GitHub runner，因此**远程CI实跑待部署验收**；本地真实矩阵与远程执行分开报告。不要将source job通过解释为完整后端CI通过。

## 3. 契约和静态资源

- [HTTP契约](../../contracts/chat-workspace.openapi.json)：详细记录新增接口、错误/分页/权限、兼容字段；旧端点是兼容索引，不冒充完整历史OpenAPI。
- [WS/SSE契约](../../contracts/realtime-contracts.json)：命令、ACK/已读语义、新偏好事件的接收范围与版本规则。
- `frontend_release.py`为整个原生模块图计算统一摘要，写入所有静态与动态CSS/JS引用。一个依赖改变会更新入口引用；vendor代码保持固定版本。再次运行应更新0个文件，`--check`不修改源码，旧引用会使检查失败。
- `asset-manifest.json`随前端提供；只有这一份公开JSON列入静态资源白名单，没有把任意JSON或服务器私有目录暴露为静态资源。

## 4. 状态与可恢复性

`/phase11/status/`并列显示 `/health/version` 实际运行指纹、静态资源版本、验收台账与限制。内容指纹按公开源码计算，统一换行；不读取环境密钥或运行数据。运行版本与静态资源是不同对象，旧服务会明确显示缺少版本信息，不能从新页面推断二进制已经升级。

清理只移除了本轮用完的三个一次性源码迁移脚本。构建目录、历史源码、依赖与不确定目录保留；详见Phase47清理清单。没有安装CI runner、下载依赖或修改系统环境。

源码同步复用公开清单，排除构建/验收产物和私人数据；目录过滤不会误排除build_stamp.py等源码文件。工程反向测试另外覆盖了退出瞬间argv清空、PID重用及打包白名单。full入口运行已注册CTest，真实HTTP/浏览器联调另有独立脚本和报告，不声称一个命令包办所有环境。
