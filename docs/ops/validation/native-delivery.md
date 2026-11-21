# 原生档交付、预检与复现

本页对应全项目意见 R1/R2。继续保留[通信与基础业务两档](reproduce.md)，在已有工具链上增加完整原生档。依赖均显式指定或从已有安装查找，脚本不会下载安装。

## 1. 四种状态分开看

| 状态 | 证据 | 不包含的结论 |
|---|---|---|
| 编译支持 | CMake 实际开关、目标和测试矩阵 | 模型已经加载、Broker 正在运行 |
| 交付齐全 | 服务与两个 Worker、许可、运行库摘要、静态资源和迁移清单 | 外部库或模型已搬到任意新电脑 |
| 运行就绪 | 配置校验、真实 `/health/ready`、Provider/Worker 的具体探测 | 回答质量、训练效果 |
| 质量验证 | 授权评估集、输出、版本、判分与回滚证据 | 仅生成一个答案或 WAV 就已通过质量验收 |

`runtime_inventory.py` 和 `release_preflight.py` 明确输出未探测的数据库连接、模型预热和质量状态。不能从 `passed=true` 推断全部 AI 外部能力已启用。

## 2. 原生构建

准备一个本机私有 CMake 初始缓存，记录已有 gRPC、Protobuf、ONNX、RocketMQ C++ SDK、sherpa、libcurl、libsodium、PostgreSQL、OpenSSL、GTest 的准确位置。缓存不提交 Key、连接密码或私人模型路径。所有外部安装继续遵守单项批准规则。

```bash
python3 scripts/project/reproduce.py native \
  --build-dir build/repro-native \
  --initial-cache /你的路径/native-dependencies.cmake \
  --postgres-prefix /home/pikachu/phase11-deps/postgresql/18.6 \
  --gtest-dir /home/pikachu/.local/lib64/cmake/GTest
```

`native` 强制真实 PG/Sodium/CURL、gRPC、ONNX、RocketMQ、sherpa 开关开启；任何缺件均失败。使用新的空构建目录，`reproduction.json` 才会记录 `builtFromEmptyDirectory=true`。检查已有构建需明确加 `--verify-existing-build`，报告保持区别。

脚本新建一次性 PostgreSQL，应用 001–024，运行独立 Tree/AI/Speech 链接测试、协议与业务契约、业务 HTTP/WS/SSE、重启恢复与 OTLP 鉴权/导出故障检查。只有未提供隔离 Redis fixture 时允许跳过 Redis 契约。不会连接原业务库。

## 3. 清单与解包

```bash
python3 scripts/project/runtime_inventory.py \
  --build build/repro-native --profile native \
  --output build/repro-native/native-inventory.json
python3 scripts/project/package_release.py package \
  --build build/repro-native --output build/delivery-native
python3 scripts/project/package_release.py verify \
  build/delivery-native/nebula-linux-x86_64.tar.gz \
  --postgres-prefix /home/pikachu/phase11-deps/postgresql/18.6
```

版本 2 交付包增加：

```text
webserver
workers/
├─ webserver-onnx-worker
└─ function3-tts-worker
native-runtime.json
release.json / reproduction.json
release_preflight.py / run-nebula.py
frontend/、tools/observability/observer/、迁移和预置面板
```

原生清单记录每个 ELF 的动态库名称、实际解析路径和摘要；包内 Worker 必须与受测程序一致并带执行权限。外部库、模型、数据库、密码、Key 和私人资料不在交付包内。目标机需准备兼容依赖；库摘要改变时明确拒绝，重新核验和构建后再交付，避免静默混用不同运行环境。

`verify --postgres-prefix` 从新目录解包，用新临时库检查真实启动、迁移、登录资源、资源版本、Observer 入口与预检。兼容验证旧版本 1 包，但旧包不能因此被视为已经包含新预检。

## 4. 启动前检查

解包到一个新的运行目录，然后：

```bash
python3 release_preflight.py --root .
# 根据你的环境显式配置数据库、token-key 和模型；以下使用独立端口。
WEB_SERVER_PORT=18096 WEB_GRPC_ADDRESS=127.0.0.1:0 python3 run-nebula.py
```

运行原业务时按[既有配置说明](../runtime/function3-runtime.md)提供变量，本页命令不代替部署审批。预检核对包文件、Worker、外部库、配置文件和成对 TLS/数据库配置，不执行迁移或写库，也不打印私密配置的值。

缺模型目录、缺 Worker、库不匹配时先解决明确错误；数据库可连接与模型可生成仍须通过实际就绪和调用核验。独立 ONNX/TTS Worker 避免把不同 ONNX Runtime 强行链接到聊天进程。

## 5. CI 与保留

源码与 Observer 契约继续在双平台执行。显式选择 `run_native` 时，在已有隔离 Linux runner 上先完成基础业务，再使用 `WEB_CI_NATIVE_CACHE` 和 `WEB_CI_POSTGRES_PREFIX` 执行原生构建与解包验收。缓存需完整列出已有依赖；CI 不补装缺件。所有工作流产物保留七天。

本轮没有触发远端工作流，也没有发布或推送；本地和 VM 验证结果见[验收记录](../../observability/validation.md#2026-11-19-全项目改进意见落地)。
