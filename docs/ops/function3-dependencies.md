# Function3：优先复用已有 Linux 安装

所有新增下载均已逐项征得同意；以下是本轮实际采用的版本，而不是未来规划版本。未安装新的 Windows 环境，没有改 PATH、系统服务或全局 CMake 配置，没有重复下载 Ollama。

| 内容 | 实际版本 / 路径 | 使用方式 |
|---|---|---|
| Java | 已有 Java 24.0.2 | 复用，不安装 |
| RocketMQ Broker | 已有 5.1.4，`/home/pikachu/rocketmq-all-5.1.4-bin-release` | 不下载获批候选 5.5.1，已有版本真实 FIFO/NORMAL 验证通过后继续复用 |
| C++ SDK | 5.1.1，`/home/pikachu/phase11-deps/rocketmq-cpp/5.1.1/install` | 官方固定提交，复用已有 gRPC/Protobuf/OpenSSL/zlib |
| Three.js | r186，`/home/pikachu/phase11-deps/three-r186` | 仅浏览器模块与 OrbitControls，无 npm |
| sherpa-onnx | 1.13.8，`/home/pikachu/phase11-deps/sherpa-onnx/1.13.8` | Linux x64 CPU 共享库，独立进程加载 |
| Kokoro | v1.1 中文 INT8，`/home/pikachu/phase11-models/kokoro-v1.1-zh` | 复用已下载且 SHA 验证的归档，不再次下载 |
| FFmpeg | 8.1.3，`/home/pikachu/phase11-deps/ffmpeg/8.1.3/install` | Linux 最小 LGPL 构建，禁 GPL/nonfree/网络输入/自动探测 |
| PostgreSQL/libsodium/curl/ONNX | 沿用 Phase11 指定目录 | 不安装另一套 |

## 1. 固定制品与摘要

- RocketMQ SDK：`da8271b572b41b627bcbbdfb5b3310c0c5e4736b`；协议定义 `bc8e1845a8b0a1b5ec828690324401a323244354`。
- sherpa Linux 库包 SHA256：`3892d184be41027e18165e67f549cd4e4cdd8dcd73ac5579e97afd55e14e30b6`。
- Kokoro 包 SHA256：`a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6`。
- FFmpeg 官方 n8.1.3 提交：`1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7`；本轮官方 GitHub 源码归档 SHA256：`a516d39ac7a92ff7c6f2c44b0338aaecf708c42f3d4febc8cb9203de6b1e2603`。
- Three 浏览器模块和 MIT 许可的固定制品记录在 `frontend/phase11/vendor/three-r186/MANIFEST.json`。

官方网页下载 FFmpeg 一度停滞，改为同一官方仓库固定版本的源码，没有改用未知第三方二进制。GitHub 自动生成的归档字节可能随服务变化，脚本按本轮已核验制品固定摘要；将来变化必须重新验证，不能删除摘要检查强行安装。

## 2. SDK 编译兼容调整

已有 Protobuf 为 35，SDK 发布代码使用较旧 Abseil/Protobuf 接口。仅在独立 SDK 源码目录做如下兼容修改：

1. C++ 标准从 11 提到 17，移除 `find_package(protobuf 3.19 ...)` 的旧版本语义约束，继续 CONFIG 查找现有安装。
2. 旧线程注解改为 `ABSL_*` 前缀，内部注解头改为公开的 `absl/base/thread_annotations.h`。
3. `google::protobuf::util::Status` 局部返回值改为 `auto`，不新增转换库。
4. 把固定版本的三个 `rocketmq-apis` 协议定义复制到 SDK 指定 proto 目录；关闭示例和 SDK 测试依赖。
5. 使用既有 `/home/pikachu/.local-grpc-systemssl`，没有 FetchContent，也没有另装 gRPC。

上游安装包内存在 5.1.0/5.0.0 元数据字符串，源码身份以固定 5.1.1 提交为准，不依据该字符串推断下载了另一个版本。独立 SDK 源码、构建、安装合计实测约 164 MiB，在获批预算内。

## 3. 已有制品的重复准备

以下脚本不访问网络：

```bash
python3 scripts/function3/dependencies/prepare_kokoro.py \
  --archive /home/pikachu/kokoro-int8-multi-lang-v1_1.tar.bz2
python3 scripts/function3/dependencies/build_ffmpeg.py \
  --archive /home/pikachu/ffmpeg-8.1.3-github.tar.gz
```

Kokoro 准备脚本先验证官方包 SHA，再安全展开并生成 377 个资源的逐文件摘要；相同文件直接复用。TTS 启用时校验清单，不在 CMake 阶段下载模型。

构建要点：

```text
WEBSERVER_ENABLE_ROCKETMQ=ON
WEBSERVER_ROCKETMQ_ROOT=/home/pikachu/phase11-deps/rocketmq-cpp/5.1.1/install
WEBSERVER_ENABLE_SHERPA_TTS=ON
WEBSERVER_SHERPA_ROOT=/home/pikachu/phase11-deps/sherpa-onnx/1.13.8
```

所有库仍为可选依赖；开启而路径缺失时明确报错，不自动补下载。

## 4. 隔离验证与卸载

本轮测试数据位于 `/home/pikachu/phase11-data/function3`，HTTP/HTTPS 使用 18094/18443，PostgreSQL 仅 Unix socket 55434。Broker 的测试端口在独立用户网络命名空间内，宿主不暴露新 Broker 监听。

停机仅使用任务自己的 PID 与标记校验脚本；先停止隔离应用，再停止私有 Broker 和私有 PostgreSQL。运行数据保留。以后卸载时，停掉这些独立进程后删除相应版本目录即可，模型、测试库、音频和审计是否删除另行决定；不要删除原 Broker 或正式数据库。

## 5. 已安装 SDK 的准确差异与优先复用

完整差异和逐文件前后 SHA 在 `scripts/function3/dependencies/patches/SdkCompatibility/`。除编译兼容外，包含明确的 FIFO 最终完成策略；专门的补丁文件保留上游许可。`reuse_sdk.py` 先核对全部 18 个变更文件及既有协议定义，相同版本直接复用，不联网。部分文件不一致会停止，不覆盖未知修改。`--rebuild` 才触发既有工具链重新编译。

项目配置核对 SDK 的策略摘要与项目内补丁一致。三个 Topic 和独立消费组通过官方管理工具预先创建；契约测试使用单独消费组，不从运行服务的消费组中抢走消息。

为给隔离验证腾出空间，只清理了已经核对且 Windows 留有相同 SHA 副本的重复下载包，以及本轮第三方构建可重新生成的对象文件。已安装引擎、模型、原 Broker、业务数据及旧日志均保留。上面的归档准备示例需要把 Windows 留存包复制回指定 Linux 路径后使用；当前运行无需重复展开或下载。

## 6. 普通页面模式的端口与防火墙

`run_server.py start --ai` 的 HTTP 端口是 **18094**，不是旧服务的18090。页面分别为 `/phase11/`、`/phase11/tree/`、`/phase11/admin/`；这个模式不启用MQ/TTS。

2026-10-24现场确认：虚拟机为192.168.239.135，服务器监听0.0.0.0:18094，本机 `/health/live` 返回200，但Windows连接超时。ens160属于firewalld的public区域，原规则只放通18090，没有18094。已添加下面的**运行时限定来源规则**，只允许Windows VMware网卡192.168.239.1访问18094；没有关闭防火墙或修改其他端口：

```bash
sudo firewall-cmd --zone=public --add-rich-rule='rule family="ipv4" source address="192.168.239.1/32" port port="18094" protocol="tcp" accept'
```

该规则不是永久规则，防火墙reload或虚拟机重启后需重新添加。只有Windows来源地址确实是192.168.239.1时才能照抄；网络地址变化时先核对来源。撤销本条规则使用相同参数，将 `--add-rich-rule` 替换为 `--remove-rich-rule`。

修正后已从Windows验证健康检查及以上三个HTML页面全部返回HTTP200。若本机健康检查正常、Windows仍超时，应先检查端口、网卡区域、防火墙和代理，不能据此反复重启应用或重装数据库。

完整 `--with-mq --tts --ai` 模式仍运行在私有网络namespace；本规则不会为该模式自动建立跨namespace的页面访问入口。

## 7. 原18090账号与当前测试库的区别

`run_server.py` 是隔离验证脚本：current实例固定连接 `/home/pikachu/phase11-data/function3/pg/run`、端口55434、数据库 `function3_runtime_test`。旧 `webserver-user-service.sh` 默认连接 `/home/pikachu/phase11-data/postgresql/run`、端口5432、数据库 `phase11`。端口本身不决定账号是否共用，真正决定因素是应用连接的数据库。

因此旧18090账号不会自动出现在当前18094页面。用户名和密码是在当前库的账号表中核验，旧浏览器Session也不能直接当作新测试环境的登录凭证。2026-10-24只读检查确认旧数据库目录仍存在，但该旧PG实例当前未运行；未启动它查询账号，不能仅凭目录存在宣称旧库内容已完整核验。

本轮没有改旧密码、复制账号或迁移旧业务数据。若需要在Function3中沿用旧账号及聊天记录，后续应先备份旧库及对应文件、密钥，再克隆独立学习库，执行新增迁移并校验关联数据，最后配置新服务连接该副本。保留原密码散列即可继续校验原密码，不需要找回密码明文；新环境通常仍需重新登录。以上数据延续操作尚未执行。
