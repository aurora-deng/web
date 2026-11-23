# 参与 NEBULA 维护

## 获取可用版本

到 GitHub Release 下载已验收的 ZIP 或离线镜像，核对同页的 SHA256SUMS 和交付验证。研究代码或提交修复时，先确认问题影响的发布标签，再以当前维护分支为开发基线。 [Function3.6 下载](https://github.com/aurora-deng/web/releases/tag/Function3.6) · [部署与回退](https://github.com/aurora-deng/web/blob/Function3.6/docs/ops/runtime/function3.6-deployment.md)

## 复现和提交

1. 用问题表单记录 Git 提交、环境、触发步骤、预期和实际结果。日志、调用链和截图先去掉账号、Cookie、数据库连接串、密钥和私人内容。
2. 按业务说明定位路径，在隔离环境使用临时数据。源码检查命令为 `python scripts/project/ci_checks.py source`，Web、真实 PostgreSQL 与车队 gRPC 分别按复现文档执行。
3. 业务改动补充角色、失败路径及重试验证。涉及配置、迁移或上线方式时说明回退步骤。接口和业务文档随代码更新。
4. 发布制品、校验和、验收摘要使用对应标签的 Release 页面。历史发布标签用于复现原构建。

[业务与实操](https://github.com/aurora-deng/web/blob/Function3.6/docs/business/README.md) · [本地复现](https://github.com/aurora-deng/web/blob/Function3.6/docs/ops/validation/reproduce.md) · [维护台账](https://github.com/aurora-deng/web/blob/Function3.6/docs/business/release-matrix.md)
