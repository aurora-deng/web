# Phase 11 依赖审批

本文记录每个依赖的审批与实施状态。每一种依赖都要单独得到许可；一个依赖获准，不会自动
批准后面的依赖。

## 第一审批点：PostgreSQL 与 libpq

**状态：已批准、已安装、已验证。**

用户选择 Linux 虚拟机 `192.168.239.135`，全部内容放在 `/home/pikachu`。虚拟机是 CentOS
Stream 9，为了满足“安装环境都放在用户目录”的要求，本次没有使用 `dnf`、没有创建 root
服务，而是从 PostgreSQL 官方源码构建：

| 项目 | 实际值 |
|---|---|
| PostgreSQL/libpq | 18.6，官方 SHA-256 校验通过 |
| 安装前缀 | `/home/pikachu/phase11-deps/postgresql/18.6`（34 MB） |
| 源码与构建树 | `/home/pikachu/phase11-deps/src/postgresql-18.6`（308 MB） |
| 数据与日志 | `/home/pikachu/phase11-data/postgresql`（当前约 73 MB） |
| 正式/测试数据库 | `phase11` / `phase11_test` |
| 网络 | 只监听 `127.0.0.1:5432`，不开放虚拟机外部访问 |
| 认证 | 应用角色 `phase11_app`，SCRAM；口令文件权限 `0600` |
| 系统影响 | 不改 `PATH`，不注册 systemd，不触碰 MySQL 3306 与 Redis 6379 |
| 验证 | PostgreSQL 核心 231/231；Repository 契约、扩展契约、完整服务器构建与启动；当前 Debug/Release/Sanitizer CTest 均为 92/92 |

日常启停使用 [postgres-user-service.sh](../../scripts/phase11/postgres-user-service.sh)，迁移使用
[postgres-migrate.sh](../../scripts/phase11/postgres-migrate.sh)。脚本不含口令。

卸载时先停止实例，再移除安装前缀；源码树可以独立删除。数据目录、日志、密钥和备份属于
用户数据，任何删除都必须再次明确确认，本项目不会自动清理。

### 用途

- PostgreSQL Server：持久保存用户、Session、好友、会话、消息、Outbox、模型注册和训练授权；
- `libpq` 与头文件：C++ PostgreSQL 适配器使用的官方 C 客户端；
- `psql`：运行迁移、检查数据和执行真实数据库契约测试。

当前选择 `libpq`，不额外引入 `libpqxx`。业务层已经有 C++ Repository 接口，驱动内部再加
一层 C++ 包装即可；此时引入 `libpqxx` 会多出一个需要单独管理版本和安装位置的依赖。

### 版本建议

建议 PostgreSQL **18 的当前补丁版本**，服务端、客户端和开发头文件保持同一来源。PostgreSQL
官方当前列出的稳定补丁版是 18.6，18 系列支持到 2030-11-14。不要使用 PostgreSQL 19 Beta。

官方资料：

- [PostgreSQL 版本支持周期](https://www.postgresql.org/support/versioning/)
- [Ubuntu 安装说明](https://www.postgresql.org/download/linux/ubuntu/)
- [Windows 安装说明](https://www.postgresql.org/download/windows/)

### 位置选项

#### A. Linux 虚拟机（推荐）

本项目的完整网络核心使用 epoll，最终构建和运行环境本来就是 Linux。建议在虚拟机安装
`postgresql-18`、`postgresql-client-18` 和 `libpq-dev`，然后在同一环境完成连接池、迁移、
契约测试与故障测试。

- 二进制/头文件：由系统包管理器放入 `/usr`；
- 配置：Ubuntu 常见位置为 `/etc/postgresql/18/main`；
- 数据目录：可使用系统默认，也可由用户指定独立磁盘目录；执行前必须再次确认；
- 预计下载：约 50–150 MB，随发行版和依赖变化；
- 预计安装：约 200–500 MB，数据库数据另算；
- 系统影响：创建 `postgres` 系统账号、数据库集群和 systemd 服务，默认监听端口 5432；
- 软件源影响：若发行版仓库没有 18，需要另行批准加入 PostgreSQL 官方 PGDG 软件源；
- 卸载：用发行版包管理器移除软件包；数据目录和备份是否删除必须另行确认，不自动清理。

#### B. Windows 自定义目录

使用 PostgreSQL 官网指向的 EDB 安装器，或使用其二进制 ZIP。安装器包含服务器、pgAdmin 和
StackBuilder；本项目只需要服务器、`psql`、`libpq` 与开发文件，pgAdmin 可不装。

- 程序位置：由用户给出绝对目录，例如 E 盘某个工具目录；
- 数据位置：由用户单独给出绝对目录，不能与程序目录混在一起；
- 预计下载：约 300–500 MB（取决于是否包含 pgAdmin）；
- 预计安装：约 600 MB–1.5 GB，数据库数据另算；
- 系统影响：安装器模式通常创建 Windows 服务；ZIP 模式可手动初始化，不必修改全局 PATH；
- 网络影响：仅本机学习时绑定 loopback，不需要开放公网防火墙；
- 卸载：安装器模式使用卸载程序并检查服务；ZIP 模式先停止实例再移除程序目录。数据目录
  是否删除必须单独确认。

#### C. 暂不安装

保留 `InMemoryStore`、PostgreSQL 接口和迁移脚本，可以继续做不依赖真实数据库的设计学习，
但无法验证连接池、SQL 事务隔离、重启持久化、并发唯一约束和真实数据库契约。

## 第二审批点：libsodium

**状态：已批准、已安装、已验证。**

| 项目 | 实际值 |
|---|---|
| 版本 | 1.0.22 |
| 安装前缀 | `/home/pikachu/phase11-deps/libsodium/1.0.22`（约 1.9 MB） |
| 安装方式 | 官方发布源码，用户目录构建，不使用 root |
| 系统影响 | 不改 `PATH`/`LD_LIBRARY_PATH`，不注册服务，不修改系统库 |
| 项目接入 | `SodiumCredentialCodec`，由 `WEBSERVER_SODIUM_ROOT` 显式定位 |
| 验证 | libsodium 官方测试 101/101；Phase 11 Debug/Release/Sanitizer 与真实登录黑盒通过 |

### 用途

- `crypto_pwhash_str_alg(..., crypto_pwhash_ALG_ARGON2ID13)`：保存 Argon2id 密码散列；
- `crypto_pwhash_str_verify`：登录时验证密码；
- `randombytes_buf`：生成不可预测的 Session/CSRF Token；
- `crypto_generichash` 和 `sodium_memcmp`：Token Hash 与常量时间比较。

它会实现 `ICredentialCodec`。可以把它理解成保险库里经过检验的锁芯：AuthService 决定“何时
锁门”，libsodium 负责真正可靠地造钥匙、验钥匙，项目不自己发明密码算法。

### 已采用的版本与安装方法

采用 **1.0.22**，从官方发布源码构建到：

```text
/home/pikachu/phase11-deps/libsodium/1.0.22
```

构建不需要 root，不注册服务，不修改全局 `PATH`、`LD_LIBRARY_PATH` 或系统 CMake 配置。
项目通过 `WEBSERVER_SODIUM_ROOT` 显式找到它。

参考：

- [libsodium 官方安装文档](https://doc.libsodium.org/installation)
- [libsodium 官方 GitHub Releases](https://github.com/jedisct1/libsodium/releases)

卸载时删除该版本安装前缀即可；它没有数据库、后台服务或注册表内容。源码和构建树是否一并
删除仍单独决定。

## 第三审批点：libcurl 开发库

**状态：已批准、已安装、已接入并验证。**

系统原有 `curl 7.76.1` 缺少开发头文件。本次按照用户批准的位置，从 curl 官方签名源码独立
构建，不替换系统 curl：

| 项目 | 实际值 |
|---|---|
| curl/libcurl | 8.22.0（2026-09-02 发布） |
| 安装前缀 | `/home/pikachu/phase11-deps/curl/8.22.0`（3.8 MB） |
| 源码 / 构建树 | 33 MB / 15 MB |
| SHA-256 | `f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7` |
| OpenPGP 签名 | 有效；签名密钥指纹 `27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2` 与 curl 官方校验页一致 |
| 编译能力 | HTTP/HTTPS、OpenSSL、异步 DNS、IPv6、线程安全、Unix Socket、zlib |
| 禁用项 | curl 命令行安装、FTP 等非 HTTP 协议、brotli、zstd、libpsl、HTTP/2；Ollama Provider 不需要这些能力 |
| 系统影响 | 不修改 `PATH`/`LD_LIBRARY_PATH`，不注册服务，不替换系统库 |
| 项目定位 | `WEBSERVER_CURL_ROOT=/home/pikachu/phase11-deps/curl/8.22.0` |
| 官方测试 | 适用于该裁剪构建的 unit tests **70/70**；其余 9 项因未编译 FTP/SFTP/Gopher/NTLM/Debug/TrackMemory 而按官方条件跳过 |

项目已实现 `OllamaModelProvider` 和 `AIChatService → ModelRouter → SSE` 纵向链路，并用本地
伪 Ollama HTTP 服务验证真实 libcurl 请求、流式 NDJSON、错误、取消、有界排队和最终消息落库。
伪服务没有模型能力，测试结论不冒充真实推理结果。

版本与签名依据：[curl 官方下载页](https://curl.se/download.html) 与
[curl 官方签名校验说明](https://curl.se/docs/verify.html)。卸载时删除该安装前缀即可；源码和
构建树可分开保留或删除，不涉及服务和用户数据。

## 第四审批点：ONNX Runtime GenAI

**状态：已批准、Linux CPU 版已安装、代码已接入、真实模型验证已通过。**

本次批准只包含两个运行库，不包含模型、CUDA、cuDNN、Ollama 或 Python 训练工具。

| 项目 | 实际值 |
|---|---|
| ONNX Runtime GenAI | 0.17.0，官方 Linux x64 CPU 预编译包 |
| ONNX Runtime | 1.28.0，官方 Linux x64 CPU 预编译包 |
| Runtime 安装前缀 | `/home/pikachu/phase11-deps/onnxruntime/1.28.0`（约 25 MB） |
| GenAI 安装前缀 | `/home/pikachu/phase11-deps/onnxruntime-genai/0.17.0`（约 37 MB） |
| 下载归档 | 9,125,960 字节 + 11,811,857 字节 |
| ORT SHA-256 | `a3e1b79d7bb1bf09696ce675f49e4064e6c81f6202b8225624fff0e93f8d6407` |
| GenAI SHA-256 | `482a17113556ff76c53820b801f391cdcbaac0b6c4089065182683c03fdbc8d9` |
| 系统影响 | 不改 `PATH`/`LD_LIBRARY_PATH`，不注册服务，不覆盖系统库，不安装 GPU 组件 |
| 项目定位 | 两个 `WEBSERVER_ONNX_*_ROOT` 显式路径，生成目标写入 RPATH |

下载后先核对官方 SHA-256，再检查归档条目没有绝对路径或 `..` 路径穿越，最后才解压。
`ldd` 已确认测试程序实际解析到上述两个用户目录中的动态库，没有 `not found`。

版本选择依据：ONNX Runtime GenAI 0.17.0 的 Linux CPU CI 使用独立 ONNX Runtime 1.28.0。
GenAI 与核心 Runtime 已拆包，Generate C/C++ API 仍为 Preview，升级版本时必须重新构建和测试，
不能只替换 `.so` 文件。

### 已实现的接入

1. `InProcessOnnxModelProvider` 复用 `IModelProvider`，`ModelRouter` 可同时注册 Ollama 与 ONNX；
2. 模型加载、聊天模板、Tokenizer 和逐 Token 生成进入专用有界 Worker，不占用 Reactor；
3. 模型目录只能使用配置根目录下的相对路径，拒绝符号链接、越界路径和特殊文件；
4. `sha256-tree-v1` 覆盖排序后的相对路径、文件大小和文件内容，数据库激活版本携带的摘要必须匹配；
5. 支持 `.onnx_adapter`、模型缓存、Adapter 缓存、Token 上限、Prompt 字节上限、取消和安全关闭；
6. 模型错误只结束当前生成，错误文本隐藏模型根路径，普通聊天继续运行；
7. 配套 `scripts/phase11/model_checksum.py` 与 C++ 使用相同摘要算法，固定夹具结果已交叉验证；
8. `webserver-onnx-worker` 可独立加载模型，聊天进程通过 `GrpcOnnxModelProvider` 的
   server-streaming 接收 Token；两端有 deadline、取消、Bearer 认证、有界队列和 TLS 生产护栏。

独立进程模式复用此前已经批准并安装的 Protobuf、gRPC、OpenSSL、ONNX Runtime 和 ONNX Runtime
GenAI，不下载新依赖。把模型搬到独立进程的目的，是让模型崩溃和内存峰值不直接终止聊天服务；
它不改变 ONNX 两个运行库的审批边界。

### 当前验证边界

- Debug、Release、ASan+LSan+UBSan 三套 Linux 全量 CTest 均为 **92/92**；
- 专项测试会真实加载 GenAI/ORT 动态库，用损坏模型验证异步失败、取消、路径逃逸拒绝、
  校验和变化、Python/C++ 摘要一致性及安全析构；
- `phase11_onnx_model_smoke` 已在 Debug/Release/Sanitizer 中编译，Release 版已实际加载
  Phi-3 Mini 4K CPU INT4 并生成 `Phase 11 onnx ready.`；
- 真实整链黑盒已通过：WebSocket 接收 `ai.generate`，PostgreSQL 选择 active version，
  ONNX Worker 生成 242 个 token / 1104 字节，SSE 输出成功终态，用户和 AI 消息均持久化；
- 独立 gRPC Worker 使用真实 Qwen Adapter 生成 512 个 token / 2424 字节；Worker 停止后，AI
  请求受控失败而普通账号、好友、聊天、ACK、SSE 和历史链路继续通过；
- Worker 生产护栏的 TLS 路径也已用临时证书、客户端 CA 和服务名校验走通，真实模型生成 241 个
  token / 1334 字节；证书私钥没有进入仓库或长期数据目录；
- 卸载时删除两个安装前缀即可；下载归档可单独删除，数据库和未来模型目录不会随之删除。

官方依据：

- [ONNX Runtime GenAI 0.17.0 Release](https://github.com/microsoft/onnxruntime-genai/releases/tag/v0.17.0)
- [ONNX Runtime GenAI 安装说明](https://onnxruntime.ai/docs/genai/howto/install.html)
- [ONNX Runtime GenAI C++ API](https://onnxruntime.ai/docs/genai/api/cpp.html)
- [ONNX Runtime 1.28.0 Release](https://github.com/microsoft/onnxruntime/releases/tag/v1.28.0)

## 后续审批点（当前不安装）

| 依赖 | 用途 | 到达阶段 |
|---|---|---|
| Windows Ollama（已由用户安装） | 实际生成验证 | 只配置可达 API 地址，不在 Linux 重复安装 |
| Olive/PEFT 等 Python 工具 | LoRA 训练、转换与 `.onnx_adapter` | Adapter 训练流程 |

## 第五审批点：Phi-3 Mini 4K CPU INT4 真实模型

**状态：已批准、已下载、已逐文件校验、真实生成与整链黑盒已通过。**

Microsoft 的 ONNX Runtime GenAI 官方 CPU 教程使用
`microsoft/Phi-3-mini-4k-instruct-onnx` 中的
`cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4`。它自带 `genai_config.json`、Tokenizer 和
外部权重，模型许可为 MIT，适合作为当前 CPU Provider 的第一条成功生成验证。

| 项目 | 实际值 |
|---|---|
| 用途 | 验证真实模型加载、逐 Token 输出、取消、并发、关闭和端到端 SSE |
| 官方仓库 | `microsoft/Phi-3-mini-4k-instruct-onnx` |
| 子目录 | `cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4` |
| 许可 | MIT |
| 固定 revision | `5f5f794c1c23c9d5ee142af85df02a6cc52d6945` |
| 下载/落盘 | 10 个官方模型文件，2,725,547,235 字节；不解压第二份归档 |
| 位置 | `/home/pikachu/phase11-models/phi3-mini-4k-instruct-cpu-int4` |
| 目录指纹 | `sha256-tree-v1:dd0a9ea96525ac10d1875f889548c4d051a330b301bdd1cf6b9f4b5eaa220094` |
| 下载方式 | 使用现有 curl 按固定 revision 直接下载；不安装 pip、Git LFS 或 Hugging Face CLI |
| 系统影响 | 不改 PATH、不注册服务、不启动后台进程；CPU 推理会占用较多内存和计算时间 |
| 删除方式 | 停止使用该模型版本后删除上述单一模型目录；数据库绑定需先切换或停用 |

虚拟机无法直接连接 `huggingface.co`，因此通过 Hugging Face 镜像代理传输。下载程序先校验
文件名只能在固定子目录内，然后使用 `.part` 断点续传，完成后按模型 revision 元数据中的
LFS SHA-256 或 Git blob SHA-1 逐文件校验，通过后才原地改名。全部清单在模型目录的
`DOWNLOAD_MANIFEST.json`。

下载前根分区可用约 5.1 GB，完成后可用约 **2.6 GB**、使用率 **94%**。用户指定了
`/home/pikachu` 且批准继续，本次没有删除旧构建或用户数据。后续下载 Adapter/训练集前必须再次
检查空间，不应在当前根分区直接追加数 GB 资产。

实测结果：

- Release C++ smoke 成功生成 `Phase 11 onnx ready.`，输出 21 字节；
- 真实 HTTP + WebSocket + SSE + PostgreSQL 黑盒生成 242 个 token、1104 字节；
- 首次推理跨越 WebSocket 心跳周期，黑盒客户端已按协议回 Pong 后继续读取业务消息；
- 该次功能验证观测到服务器 RSS 约 3.56 GiB；它不是并发性能或模型质量基准。

官方依据：

- [ONNX Runtime GenAI Phi-3 CPU 教程](https://github.com/microsoft/onnxruntime-genai/blob/main/examples/python/phi-3-tutorial.md)
- [Microsoft Phi-3 ONNX 模型目录与大小](https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-onnx/tree/main/cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4)
- [Microsoft Phi-3 模型卡与 MIT 许可](https://huggingface.co/microsoft/Phi-3-mini-4k-instruct)

按用户要求不在 Linux 安装 Ollama。本次批准只覆盖表中这一个 Phi-3 模型，不自动扩展到
其他模型、Adapter、训练集或 Olive/PEFT。到达相应阶段时，仍会重新给出名称、版本、来源、
位置、空间、环境影响和删除方法，得到明确许可后再下载。

## 第六审批点：Olive / PEFT Adapter 工具链

**状态：CPython、libffi、Python Adapter 包组和 Qwen 学习模型均已批准、已安装/下载并完成真实整链验证。**

代码侧已经完成 Adapter 相对路径限制、真实 Runtime 试装、失败时保留旧 Active、成功切换和
回滚。Python 工具链、固定 revision 的 Qwen 基础模型、Adapter-ready INT4 图和独立
`.onnx_adapter` 也已完成。该状态只覆盖工程学习夹具；新的模型、训练集、GPU 工具或真正的微调任务
仍是各自独立的审批点。

当前 Debug、Release、ASan/LSan/UBSan 均为 **92/92**；真实 Phi-3 模型继续通过普通生成。
新增 Qwen Adapter 已通过图结构检查、同图 logits 对照、C++ Provider 试装/生成以及
WebSocket → PostgreSQL → ModelRouter → ONNX → SSE → Outbox 纵向黑盒。

本审批点的边界如下：

| 项目 | 待确认内容 |
|---|---|
| 用途 | Olive 转换 Adapter；PEFT 读取/训练 LoRA；PyTorch 提供张量与训练运行时 |
| 版本组合 | 以 ONNX Runtime GenAI 0.17.0、Python 3.12 和目标模型实际兼容性锁定，不盲目追最新版本 |
| 安装方式 | `/home/pikachu/phase11-deps/python-envs/lora` 独立 venv；项目 CMake 不联网 |
| 模型与输出位置 | `/home/pikachu/phase11-models/adapter-learning`，每个模型另建固定版本目录 |
| 磁盘占用 | Python wheel/venv 预计超过 1 GB；基础 HF 模型、转换中间文件和新 ONNX 输出还会增加数 GB |
| 环境影响 | 不改系统 Python，不改全局 PATH，不注册服务；只通过显式 venv 命令使用 |
| 删除方式 | 删除独立 venv、获批的源模型/中间文件和 Adapter 输出；不删除当前运行模型与数据库 |

当前项目可以并行使用 Ollama、无 Adapter 的 Phi-3 ONNX 和带 Adapter 的 Qwen ONNX。教学 Qwen
已经验证格式和运行时兼容性，但随机权重没有训练质量；生产模型仍必须逐个固定版本、校验和、评估并审批。

### 6.1 已批准子步骤 A：用户目录 CPython

虚拟机原来只有系统 Python 3.9.23，而 Olive 0.13.0 和 PEFT 0.21.2 都要求 Python 3.10 以上。
建议先只安装 Python 本身，Python 包工具链仍留到下一次单独审批。

| 项目 | 建议值 |
|---|---|
| 依赖 | CPython 3.12.15，Linux 源码版 |
| 用途 | 为 Olive/PEFT 创建隔离 venv；不替换 CentOS 系统 Python 3.9 |
| 来源 | Python.org 官方源码，3.12.15 是 2026-09-30 发布的安全修复版 |
| 下载 | XZ 源码约 20.8 MB，先核对 Python.org 公布的 SHA-256 |
| 安装位置 | `/home/pikachu/phase11-deps/python/3.12.15` |
| 构建位置 | `/home/pikachu/phase11-deps/build/python-3.12.15` |
| 空间估计 | 源码、构建和安装合计约 0.4–0.8 GB；完成后保留构建树便于审计，是否清理由用户决定 |
| 环境影响 | 不使用 root、不改全局 PATH、不覆盖 `/usr/bin/python3`、不注册服务 |
| 卸载 | 删除上述 Python 安装前缀；其 venv 在后续获批后使用独立目录，可单独删除 |

Python 安装完成后，先验证 `ssl`、`venv`、`sqlite3`、`bz2`、`lzma` 和 `ctypes` 是否完整，再给出
Olive/PEFT/PyTorch CPU 的精确解析清单和第二次审批，不把两次许可合并。

2026-10-04 实机检查确认：计划目录、`~/.local`、`/usr/local/bin` 和系统目录均不存在
`python3.12`，虚拟机仍只有 `/usr/bin/python3` 3.9.23，因此需要执行这一步。构建前的开发库审计发现
OpenSSL、zlib、bzip2、xz 和 SQLite 头文件齐全，但缺少 `libffi` 开发头文件；直接编译会使
Python 的 `_ctypes` 模块缺失，不能满足本阶段验收条件。

安装结果：源码 SHA-256 为
`c2c4321961fab0fb999d66e0cecf521c2ab3994c7992873ea99e306c1094fd5a`，与 Python.org 发布值一致；
`make altinstall` 只创建 `python3.12`，没有覆盖系统 `python3`。新解释器报告 Python 3.12.15，
`venv` 内的 pip 为 25.0.1。安装前缀实际占用约 376 MB，保留的 Python 构建树约 411 MB。

`test_ctypes`、`test_ssl`、`test_sqlite3`、`test_bz2`、`test_lzma`、`test_venv` 共 6 个测试文件、
1,439 项测试全部通过；其中 111 项按 CPython 测试条件跳过。构建缺少的可选模块为 `readline`、
`curses`、Tk、gdbm、nis 和 `_uuid`，它们不影响本阶段的隔离 venv、HTTPS、PyPI wheel、SQLite、
压缩包或 `ctypes`，因此没有扩大依赖安装范围。

### 6.2 已批准子步骤 B：用户目录 libffi

`libffi` 可以理解为 Python 和 C 函数调用约定之间的“翻译插座”。`ctypes` 需要通过它在运行时
调用动态库。系统已有运行库并不等于有编译所需的 `ffi.h`，本机检查也没有发现可复用的用户目录副本。

| 项目 | 建议值 |
|---|---|
| 依赖 | libffi 3.8.0，Linux 源码版 |
| 用途 | 先提供 `ffi.h` 和用户目录运行库，再让 CPython 3.12.15 构建完整 `_ctypes` |
| 来源 | libffi 官方 GitHub Release；下载发布归档并校验摘要 |
| 安装位置 | `/home/pikachu/phase11-deps/libffi/3.8.0` |
| 构建位置 | `/home/pikachu/phase11-deps/build/libffi-3.8.0` |
| 空间估计 | 下载约数 MB；源码、构建、测试和安装合计预计小于 100 MB |
| 环境影响 | 不使用 root、不改全局 PATH、不覆盖系统 libffi、不注册服务；仅在构建 Python 时显式传入路径 |
| 卸载 | 删除上述 libffi 安装前缀；构建目录是否清理由用户决定 |

安装结果：通过 GitHub Release API 取得发布资产的 SHA-256
`7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4`，下载文件校验一致。
上游 `make check` 因系统没有 DejaGnu `runtest` 而跳过；没有为测试框架擅自增加安装。替代验证包括：

- 真实 C 程序通过 `ffi_prep_cif` 和 `ffi_call` 调用 `add(19, 23)`，结果为 42；
- Python `_ctypes` 实际解析到 `/home/pikachu/phase11-deps/libffi/3.8.0/lib64/libffi.so.8`；
- CPython 的完整 `test_ctypes` 测试文件通过。

libffi 安装前缀实际占用约 184 KB，保留的构建树约 8.5 MB；两项安装完成后根分区仍有约 22 GB
可用空间。系统 `/usr/bin/python3` 仍报告 3.9.23，独立解释器报告 3.12.15。

### 6.3 已批准子步骤 C：Python Adapter 包组

这一组只建立 CPU 学习、转换和兼容性验证环境，不下载 Hugging Face 模型，也不在无 GPU 的虚拟机上
宣称完成 QLoRA 训练。使用 Olive 的 `lora` 与 `cpu` extras，避开 `finetune` extra 自动引入的
bitsandbytes、Triton 等 GPU 训练依赖。

| 项目 | 建议值 |
|---|---|
| venv | `/home/pikachu/phase11-deps/python-envs/lora` |
| Olive | `olive-ai[lora,cpu]==0.13.0` |
| PyTorch | `torch==2.10.0+cpu`，只从 PyTorch 官方 CPU wheel 索引取包 |
| Transformers | `transformers==5.3.0` |
| PEFT | `peft==0.21.2` |
| Accelerate | `accelerate==1.15.0` |
| SciPy | `scipy==1.18.1` |
| OGA Python | `onnxruntime-genai==0.17.0`，与项目 C++ Runtime 主版本保持一致 |
| 其他包 | Olive 声明的 ONNX、ONNX Script、NumPy、Pandas、Optuna、Pydantic、Telemetry 等传递依赖 |
| 下载估计 | CPU Torch 约 180 MB；全组 wheel/元数据预计约 0.35–0.7 GB，不下载 CUDA wheel |
| 安装占用 | venv 预计约 1.2–2.5 GB，不包含基础模型、Adapter 或转换中间模型 |
| 环境影响 | 不使用 root、不改全局 PATH、不改 CMake；仅显式调用该 venv |
| 卸载 | 删除 `/home/pikachu/phase11-deps/python-envs/lora`；下载缓存是否清理由用户决定 |

安装时先明确安装 CPU Torch，再安装其余固定顶层版本，生成 `pip freeze`、`pip check` 和安装报告；
如果解析结果尝试引入 CUDA、bitsandbytes 或 Triton，将停止而不是继续下载。

2026-10-04 安装与验证结果：

- 独立 venv 位于 `/home/pikachu/phase11-deps/python-envs/lora`，实际占用约 1.6 GB；pip 下载缓存约
  343 MB，未擅自清理；安装结束后根分区约有 20 GB 可用空间；
- 固定版本的 8 个顶层包全部安装成功，`torch==2.10.0+cpu` 报告 `torch.version.cuda is None`，
  `torch.cuda.is_available()` 为 false；
- 最终环境共锁定 71 个包，锁文件 SHA-256 为
  `f1c289322de17c4f1fc26151138e13371e57da9c1d1c06f405354e91d31624bd`；
- `pip check` 无破损依赖；再次枚举安装分发包时没有发现 `nvidia-*`、`triton` 或
  `bitsandbytes`；
- Olive、PyTorch、Transformers、PEFT、Accelerate、SciPy、ONNX Runtime GenAI 和 Requests
  均可导入；`olive --help`、`olive generate-adapter --help`、
  `olive convert-adapters --help` 均正常退出；
- 第一次导入 Olive 时发现它的 telemetry 代码实际使用 `requests`，但 Olive 0.13.0 发布元数据
  没有把该包声明为依赖。为了使环境可重复构建，项目显式固定 `requests==2.34.2` 后重新完成了
  上述全部检查；
- 这一步没有下载 Hugging Face 模型、训练集或 `.onnx_adapter` 文件，也没有在 Linux 安装
  Ollama。

可重复构建文件：

- [install-lora-toolchain.sh](../../scripts/phase11/install-lora-toolchain.sh)：只有显式传入
  `--install` 才会联网安装，并执行 CPU 包审计、导入检查和 Olive CLI 检查；
- [lora-constraints.txt](../../scripts/phase11/lora-constraints.txt)：审批过的顶层版本边界；
- [lora-requirements.lock.txt](../../scripts/phase11/lora-requirements.lock.txt)：本次 Linux 实机环境的
  完整 71 包锁定快照。

安装后又对当前 Phi-3 ONNX 图做了不加载 2.7 GB 外部权重的结构审计：图共有 66 个输入，分别是
`input_ids`、`attention_mask` 和 32 层 KV Cache 的 key/value；全部图输入、初始化器和节点名称中，
包含 `lora` 或 `adapter` 的标记为 **0**。因此当前图只能作为普通生成模型使用，不能把一个
`.onnx_adapter` 直接塞进去。审计脚本是
[inspect-onnx-adapter-inputs.py](../../scripts/phase11/inspect-onnx-adapter-inputs.py)，无标记时以退出码 3
明确区分“图读取失败”和“读取成功但没有 Adapter 接口”。

### 6.4 已完成子步骤 D：Qwen Adapter 学习模型资产

为了验证“基础模型 → Adapter 输入图 → `.onnx_adapter` → C++ 运行时切换”的完整流程，建议使用
Olive 官方快速入门采用的 `Qwen/Qwen2.5-0.5B-Instruct`。它比现有 3.8B Phi-3 更适合 6 GiB 内存的
学习虚拟机，同时能避免下载来源不明的第三方 Adapter。

| 项目 | 建议值 |
|---|---|
| 基础模型 | `Qwen/Qwen2.5-0.5B-Instruct` |
| 固定 revision | `7ae557604adf67be50417f59c2c2f167def9a775`，不跟随会漂移的 `main` |
| 许可 | Apache-2.0 |
| 官方文件大小 | 10 个文件共 999,604,126 字节；其中 `model.safetensors` 为 988,097,824 字节 |
| 权重摘要 | `model.safetensors` SHA-256：`fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe` |
| 用途 | 用 Olive 生成带 Adapter 输入的 ONNX 图，并创建固定随机种子的教学 Adapter；验证加载、激活、回滚和输出差异，不冒充训练质量 |
| 模型位置 | `/home/pikachu/phase11-models/adapter-learning/qwen2.5-0.5b-instruct/source` |
| 输出位置 | `/home/pikachu/phase11-models/adapter-learning/qwen2.5-0.5b-instruct` 下的 Olive 中间目录和 `olive-int4-adapter-deploy` |
| 缓存位置 | `/home/pikachu/phase11-deps/cache/huggingface`，不写系统目录 |
| 空间预算 | 原始模型约 1.0 GB；连同缓存、ONNX 导出和中间文件预留 6–10 GB；当前根分区约有 20 GB 可用 |
| 资源风险 | 虚拟机为 4 核、6.0 GiB 内存和 2.1 GiB Swap；导出可能较慢或触发内存不足，失败时保留日志并停止，不扩大 Swap 或改系统配置 |
| 环境影响 | 不安装服务、不修改 PATH、不触碰现有 Phi-3、PostgreSQL 或 nginx；不安装 Linux Ollama |
| 卸载 | 删除上述 Qwen source/output 目录与独立 Hugging Face 缓存；不删除现有模型和 Python 工具链 |

完成结果：

- 10 个官方文件共 999,604,126 字节全部按固定 revision 的 Git/LFS 元数据校验；
  `model.safetensors` SHA-256 为
  `fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe`；
- 创建固定种子的非零 PEFT 学习 Adapter 和形状相同的全零对照，各有 192 个张量、540,672 个参数；
- Olive 0.13 与 OGA 0.17 的旧模块名、参数准备 API 不一致，由项目内
  [olive-oga017-compat.py](../../scripts/phase11/olive-oga017-compat.py) 运行时桥接；没有改
  `site-packages`，也没有新增依赖；
- FP32 图导出成功，但 Adapter 抽取达到约 6.8 GiB RSS 后被 OOM 终止；INT4 ModelBuilder 导出约
  400 MiB 图，峰值约 4.9 GiB，抽取峰值约 2.1 GiB，适合当前虚拟机；
- 最终 Adapter-ready 图有 818 条输入声明、434 个唯一名称，其中 384 个是唯一 LoRA 权重/scale；
  Olive 对每个 Adapter 输入产生了两条声明，OGA 0.17 Runtime 已实际加载通过；`.onnx_adapter`
  为 1,241,136 字节，SHA-256 为
  `df04b073248100c214a66fbbc42375907e600de5447c5e490e7c62af4689906a`；
- 零/非零 Adapter 使用相同的 `model.onnx`（SHA-256 均为
  `518cb7afd088ab5a297bf37152b8fc984cec6c93daa3635705e121dfc1e30a25`），关闭采样后
  151,936 个 logits 全部变化，最大绝对差为 `1.047957420349121`；
- C++ `InProcessOnnxModelProvider` 完成真实试装与生成；数据库 Active 版本记录了相对
  `adapter_artifact`，纵向黑盒收到 512 个 token、3315 字节，AI 消息保存了模型/版本/Adapter
  追踪信息，相关 Outbox 待发布数为 0；
- 干净部署目录摘要为
  `sha256-tree-v1:68d72d86fe15b45f69f497ab41c43c6722466b89ee76a3ed4c6a392d8bae1c41`。

这一步只验证 Adapter 的工程机制。真正让模型学会某种知识或风格还需要授权训练数据、质量评估和
GPU 微调；官方教程也把本地 LoRA/QLoRA 微调列为 NVIDIA GPU 工作负载，因此本次没有在 CPU
虚拟机上把随机权重描述成有质量意义的训练成果。
