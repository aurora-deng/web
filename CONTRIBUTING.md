# 参与 NEBULA 维护

## 获取可用版本

从版本索引确认源码标签、验证范围及是否有二进制。下载 ZIP 或离线镜像时核对同一发布页的校验和。分支里的源码说明、复跑步骤和测试结果随该版本更新。 [版本索引](docs/releases/README.md) · [Function3.6 历史制品](https://github.com/aurora-deng/web/releases/tag/Function3.6)

## 复现和提交

1. 用问题表单记录 Git 提交、环境、触发步骤、预期和实际结果。日志、调用链和截图先去掉账号、Cookie、数据库连接串、密钥和私人内容。
2. 按业务说明定位路径，在隔离环境使用临时数据。源码检查命令为 `python scripts/project/ci_checks.py source`，Web、真实 PostgreSQL 与车队 gRPC 分别按复现文档执行。
3. 业务改动补充角色、失败路径及重试验证。涉及配置、迁移或上线方式时说明回退步骤。接口和业务文档随代码更新。
4. 发布制品、校验和、验收摘要使用对应标签的 Release 页面。历史发布标签用于复现原构建。

## 发布约定

新功能使用 `release/v0.<次版本>-<英文主题>` 分支和同名 `v0.<次版本>-<英文主题>` 注释标签。切分提交时保留独立的功能、测试和文档改动。标签固定到验证过的源码。发布说明写清使用入口、数据或配置影响、测试范围及制品类型。需要修改业务默认行为时，先用变更分支和 PR 保留审查记录。 [GitHub 发布文档](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) · [PR 管理参考](https://docs.github.com/en/pull-requests/reference/managing-and-standardizing-pull-requests)

[业务与实操](docs/business/README.md) · [本地复现](docs/ops/validation/reproduce.md) · [维护台账](docs/business/release-matrix.md)
