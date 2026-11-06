# 从干净目录复现 Nebula

复现分为两个级别。两者都从源码配置、构建、启动，再访问真实 HTTP 服务。源码检查仍可以单独在 Windows 运行。

| 级别 | 已有工具要求 | 能证明什么 |
|---|---|---|
| `transport` | Linux、支持 C++20 的编译器、CMake/CTest 3.21+、Make 或 Ninja、Python 3.9+、匹配的 OpenSSL 与 nghttp2 开发库 | 最小服务能启动、响应、提供正确静态资源、正常退出；账号 API 在未配置时明确返回 503 |
| `business` | 上述工具加 GoogleTest、PostgreSQL 开发库和可执行文件、libsodium | 从 001–024 建库，运行通信/业务/真实 PG 测试，真实注册登录、WebSocket、果实幂等发布、Outbox/SSE 和重启后恢复 |

入口：[reproduce.py](../../../scripts/project/reproduce.py)；数据库与进程隔离：[repro_runtime.py](../../../scripts/project/repro_runtime.py)。脚本不会安装依赖。没有相应工具时先准备已有依赖的位置。参考环境为 Linux、GCC 11.5、CMake 3.31、PostgreSQL 18.6；这些是已用配置，不代表唯一兼容版本。

## 复现步骤

在可信或隔离的 Linux 环境执行。短时 HTTP 监听使用空闲端口；现有通信核心仍监听所有网卡，脚本只从回环地址测试，不会开放防火墙。

```bash
python3 scripts/project/reproduce.py transport --build-dir build/repro-transport

python3 scripts/project/reproduce.py business \
  --build-dir build/repro-business \
  --postgres-prefix /path/to/installed/postgresql \
  --sodium-prefix /path/to/installed/libsodium \
  --gtest-dir /path/to/installed/lib/cmake/GTest
```

`--sodium-prefix` 和 `--gtest-dir` 可省略，由 CMake 查找系统安装；如系统有 Ninja，可用 `--generator Ninja`。默认使用 Make、两路并行，可用 `--jobs 3`。如果用户目录中旧 OpenSSL 抢占了系统版本，可成对指定 `--openssl-include /usr/include --openssl-lib-dir /usr/lib64`；库目录按系统实际路径填写。

- 业务脚本创建短路径、仅当前用户可访问的临时 Unix 套接字 PostgreSQL，不接收已有数据库连接串。只执行仓库内迁移，测试完成停止自己的服务器和数据库并清理临时数据。
- 构建目录中的 `reproduction.json` 汇总工具版本、二进制与资源指纹、验证范围、跳过项和结果。日志便于定位失败；账户、测试密码、临时密钥和数据库不作为报告内容。
- 不复用旧构建目录冒充全新配置。需要核验指定 Release 构建时，用 `--verify-existing-build`，报告会把它与全新构建分开标记。
- 业务档不启用 Redis、RocketMQ、TLS 云端资源、模型和真实 TTS。Redis 契约是该档唯一允许跳过的测试，报告明确列出。额外驱动仍需各自的集成环境。

Windows 源码检查：`python scripts/project/ci_checks.py source`。已有 MinGW/Boost 的 Windows 客户端使用[本机客户端构建入口](../../../scripts/client/build_windows.py)。

## 容器与 CI

- [Dockerfile](../../../Dockerfile) 是通信层演示镜像草案，`docker build -t nebula:transport .` 会尝试获取基础镜像和软件包；尚未在本轮构建。运行命令可用 `docker run --rm -p 127.0.0.1:8080:8080 nebula:transport`，此镜像不启用 PostgreSQL 账号业务。
- [CI](../../../.github/workflows/ci.yml) 默认运行源码门禁。业务工作流须手动勾选 `run_business`，使用已有依赖、已获准的隔离 runner 与 `isolated-ci` 环境。可为环境配置 `WEB_CI_POSTGRES_PREFIX`、可选的 `WEB_CI_SODIUM_PREFIX`、`WEB_CI_GTEST_DIR`，以及成对的 `WEB_CI_OPENSSL_INCLUDE`、`WEB_CI_OPENSSL_LIB_DIR`。本机真实通过不等于 GitHub runner 已配置或执行。
- 本说明验证可构建和可重复观察到功能，未承诺不同工具链生成相同二进制，也未执行生产替换、发布回滚或性能验收。数据库变更见[迁移规则](../../database/migration-policy.md)。

## 这次的验证记录

Linux 两档都用同一份源码从空目录构建并启动。通信档核对健康接口、`/fast`、静态资源与账号 API 的 503。业务档在全新 PostgreSQL 应用 001–024；CTest 登记 113 项，112 项通过，Redis 契约按参考档配置跳过。真实 HTTP 注册登录、WebSocket 握手、果实幂等发布、Outbox/SSE 和服务器重启后继续使用同一会话并读取果实通过。Windows 源码门禁 79 项前端、23 项 Python 工程检查通过；152 份冻结的通信核心文件未改动。

复现过程中识别并处理了两个输入问题：用户目录的旧 OpenSSL 先于系统库被选中；手工源码清单漏掉 WebSocket 示例。现在文档说明如何指定一套匹配的 OpenSSL，并建议从完整 Git 检出运行，不手选源码目录。
