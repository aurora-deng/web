# Function3.6 交付件保留与虚拟机维护

对外下载从 GitHub Release 进入。Linux 运行 ZIP、源码 ZIP、离线 Web/Fleet 镜像包、SHA256SUMS 与交付验证 JSON 保持同一发布锚点。源码分支承载后续说明和 CI。使用过的构建包可以从虚拟机清理，只要已核实对外副本或可复现源码，且运行和回滚都无依赖。 [Function3.6 下载](https://github.com/aurora-deng/web/releases/tag/Function3.6) · [部署与复验](function3.6-deployment.md)

## 2026-11-23 维护记录

| 虚拟机 | 本次处理 | 实测结果 |
| --- | --- | --- |
| CentOS | 移除 87 个旧归档副本、传输包和过期压缩日志。移除 4 个无容器引用的 NEBULA 交付验收镜像 | 文件共 476,983,397 字节，磁盘可用空间增加 833,761,280 字节 |
| Ubuntu_ros | 公钥登录恢复后移除旧 Nginx 源码包与 3 个 ROS 实验传输包。结合补丁核对已展开的 136 个文件 | 文件共 1,396,941 字节，保留已展开源码、编译目录、观测证据和 ROS 工作区 |

CentOS 交付目录里的运行 ZIP 和镜像归档与 GitHub 公布的 SHA-256 一致。现有数据库、媒体、用户目录、运行程序、离线所需依赖、用于回退的唯一快照和证据保留。两台虚拟机上的逐项清单保存在对应本机维护目录。CentOS 的 18090、18095 与 18791 检查前后均无监听。

## 交付维护

1. 在仓库根按被验收的提交构建并验证。在 Release 说明注明运行范围、依赖、校验、测试与回退。发布件附在对应 Git 标签的 Release 页面。
2. 保留已部署运行目录和现场证据。重复的本机传输包在发布验证或已展开副本核对完成后清理。发现活动引用、预期摘要变化或回退用途时保留。
3. Docker 清理按应用标签和镜像 ID 核对容器引用。交付新格式时先在隔离环境加载和运行，再挂到发布页。此版本的离线镜像需要兼容的主机动态库。

项目采用 gRPC Release 的按版本发布说明与制品方式，以及 OpenTelemetry Collector 按组件标记稳定阶段的方式。NEBULA 通过业务上线台账列明各模块证据。GitHub 默认分支提供使用指南、问题表单和 PR 模板。 [gRPC 发布](https://github.com/grpc/grpc/releases) · [Collector 组件稳定度](https://github.com/open-telemetry/opentelemetry-collector/blob/main/docs/component-stability.md) · [GitHub 发布说明](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) · [业务上线台账](../../business/release-matrix.md)
