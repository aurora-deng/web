# Function3.6 部署与回退

Function3.6 提供三种交付。Linux ZIP 含 PostgreSQL 业务服务器、页面和 Observer 入口、迁移、C++ 车队程序及各自校验信息。Docker 归档含独立的 Web 和 Fleet 应用镜像，运行时从匹配的 Linux x86-64 主机只读挂载动态库。源码 ZIP 用于阅读、维护和使用已有工具链重建。 [发布对比](../../releases/function3.6.md) · [GitHub 下载页](https://github.com/aurora-deng/web/releases/tag/Function3.6)

| 文件 | 用途 |
|---|---|
| `nebula-Function3.6-source.zip` | 指定发布提交的受控源码 |
| `nebula-Function3.6-linux-x86_64.zip` | Linux x86-64 本机运行与镜像重建 |
| `nebula-function3.6-docker-images-linux-amd64.tar.gz` | `docker load` 载入 Web 和 Fleet 镜像 |
| `SHA256SUMS`、`delivery-verification.json` | 校验下载完整性，查发布提交、镜像 ID、启动结果和库摘要 |

## 一、先校验再解包

```bash
sha256sum -c SHA256SUMS
unzip -q nebula-Function3.6-linux-x86_64.zip -d runtime-function3.6
cd runtime-function3.6
python3 release_preflight.py --root .
```

全套一起下载时运行 `sha256sum -c SHA256SUMS`。只下载其中一个时，计算该文件的 SHA-256 并核对同名条目。可从源码目录使用 `scripts/project/package_release.py verify` 对 ZIP 内部文件逐个校验。Windows 可以用 PowerShell 的 `Get-FileHash -Algorithm SHA256` 校验下载，Linux 程序在 Linux 或对应虚拟机运行。

## 二、Linux ZIP 直接部署

1. 使用单独的发布目录和端口。提前准备已有 PostgreSQL、兼容动态库和一个只有服务账号可读的随机签名密钥文件。参考以下配置，按环境填写连接串和绝对路径。
2. 首次启动先用隔离库复验。切换原业务前先备份 PostgreSQL 与附件目录，核对迁移账本。运行启动检查会确认 026 版模式，迁移和备份由操作者按既有流程执行。 [数据库操作与回退](function3-runtime.md)
3. 确认健康、页面和登录，随后按网关手册从原上游切到已验证新上游。保留旧进程配置和程序以便撤回。 [网关运行](nebula-yantu-community.md)

```bash
export WEB_SERVER_PORT=18096
export WEB_PHASE11_REQUIRED=1
export WEB_FUNCTION3_ENABLE=1
export WEB_PHASE11_DATABASE='host=/你的PG套接字目录 port=5432 dbname=nebula user=nebula connect_timeout=5'
export WEB_PHASE11_TOKEN_KEY_FILE=/你的私有目录/token.key
export WEB_PHASE11_FILE_ROOT=/你的业务目录/files
export WEB_FUNCTION3_MEDIA_ROOT=/你的业务目录/media
python3 run-nebula.py
```

启动后从另一个终端检查 `/health/live`、`/health/ready`、`/health/version` 和 `/phase11/`。未登录访问 `/api/me` 应返回 401。若动态库缺失，先按系统提示指定已有安装的进程级搜索路径。CentOS 隔离验收使用 glibc 2.34、现有 PostgreSQL 18.6、libsodium 及 libcurl；详细标识见证据。Observer 使用自身 Python 进程，需要配置实际监控数据源。研途仍经专属账号服务与网关接入。 [Observer 部署](../../observability/setup.md) · [研途与 Envoy](nebula-yantu-community.md)

## 三、离线 Docker 启动

归档可以在获准的 Linux x86-64 Docker 主机离线加载。需同时解压 Linux ZIP，从该主机已有安装准备库目录。脚本核对二进制摘要，收集 `ldd` 解析到的库，并生成哈希清单。

```bash
docker image load -i nebula-function3.6-docker-images-linux-amd64.tar.gz
for role in web fleet; do
  python3 runtime-function3.6/prepare-runtime-libs.py \
    --root runtime-function3.6 --role "$role" --output "$PWD/nebula-$role-libs"
done
docker image inspect --format '{{.Id}}' nebula-web:function3.6 nebula-fleet:function3.6
```

给 Web 容器准备独立状态目录、签名密钥和专用数据库。以下例子使用主机现有 PostgreSQL Unix 套接字。把 `PG_SOCKET_DIR` 设成允许 Web 进程访问的套接字目录，`TOKEN_KEY_FILE` 指向私有密钥，两个变量都用绝对路径；`WEB_PHASE11_DATABASE` 的容器内连接串放入权限 0600 的 `web.env`，例如 `host=/pg port=5432 dbname=nebula user=nebula connect_timeout=5`。正式库如使用密码认证，按现有凭据机制注入。

```bash
mkdir -p nebula-web-state
docker run --pull=never --name nebula-web-f36 --detach \
  --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --user "$(id -u):$(id -g)" -p 127.0.0.1:18096:8080 \
  --env-file web.env \
  -e WEB_PHASE11_REQUIRED=1 -e WEB_FUNCTION3_ENABLE=1 \
  -e WEB_PHASE11_TOKEN_KEY_FILE=/token.key \
  -e WEB_PHASE11_FILE_ROOT=/state/files -e WEB_FUNCTION3_MEDIA_ROOT=/state/media \
  -v "$PWD/nebula-web-libs:/runtime:ro,z" \
  -v "$PWD/nebula-web-state:/state:Z" \
  -v "$PG_SOCKET_DIR:/pg:ro" -v "$TOKEN_KEY_FILE:/token.key:ro" \
  nebula-web:function3.6
curl -fsS http://127.0.0.1:18096/health/ready
curl -fsS http://127.0.0.1:18096/health/version
```

SELinux 可能限制主机日常 PostgreSQL 套接字的容器访问。正式连接可改用专供容器访问的已授权 TCP/TLS 端点与对应凭据，按已有 PostgreSQL 和防火墙规则配置；避免重新标记原服务目录。隔离验收只重标记临时套接字和临时密钥。Web 镜像内运行网页业务，Observer、Envoy、研途独立服务、外部语音或模型按自己的运行文档接入。

Fleet 容器挂载 `nebula-fleet-libs` 到 `/runtime`。加入 `--network none` 后，协调器、边缘和 CLI 在同一容器内使用回环协议。复验脚本完成一次真实派发、到站、幂等重试及 Agent 查询提案。手工运行的身份变量、命令和临时账本要求见 [车队启动说明](../validation/cpp-systems.md#二启动本机演示)。跨主机的现有 SSH 转发拓扑见 [跨虚拟机实验](../validation/cpp-systems-cross-vm.md)。

## 四、复验、停止与回退

以下在 Function3.6 源码根目录执行。提供本机批准使用的工具路径。第一个命令把 ZIP 解到临时目录并创建临时 PostgreSQL；第二个命令完全离线构建两个应用镜像、实际运行、保存归档并重新加载核对镜像 ID。应用程序复制自通过回归的 ZIP，基础镜像是 `scratch`。

```bash
REV=$(git rev-parse HEAD)
python3 scripts/project/package_release.py --revision "$REV" verify \
  /你的交付目录/nebula-Function3.6-linux-x86_64.zip --postgres-prefix "$PG_PREFIX"
python3 scripts/project/offline_docker.py \
  /你的交付目录/nebula-Function3.6-linux-x86_64.zip \
  --postgres-prefix "$PG_PREFIX" --output build-function36/docker-verify
```

从源码制作 ZIP 时先按 [Linux 业务档](../validation/reproduce.md)运行带 OTLP 的 `reproduce.py business`，按 [M00–M07](../validation/m00-m07.md)运行独立 Release gRPC 全套，再执行 `package_release.py --revision "$REV" package --format zip --build "$WEB_BUILD" --fleet-build "$FLEET_BUILD" --output build-function36/zip`。受控源码 ZIP 可浏览代码；需要二进制内嵌准确 Git 修订时，在对应标签的 Git 检出构建。

回退时先停止新容器 `docker stop nebula-web-f36` 或终止新 ZIP 进程，把网关指回原上游并确认旧入口健康。数据库回退需结合本次备份、写入窗口和迁移兼容性。检查容器与库目录的保留情况，再清理对应镜像标签及独立实验目录。
