# Phase43–47 验收证据

入口为 `summary.json`。功能通过与性能未通过分别记录；原始失败不删除、不由后续诊断覆盖。

| 目录/文件 | 用途 |
|---|---|
|`linux/release-ci.json`、`linux/checks/`|Linux Release、Debug、ASan/LSan/UBSan矩阵；各114项|
|`linux/workspace*.json`及其余业务报告|真实HTTP/WebSocket/PostgreSQL、迁移、隐私、AI及语音链路|
|`linux/benchmarks/`|三轮正式同配置性能对照；ACK第2轮增加23.33%，门槛未通过|
|`linux/diagnostic-on*/`|保持同步提交的独立诊断，不代替正式门槛|
|`linux/final-runtime-smoke.json`|最终运行内容指纹、迁移与静态资源一致性|
|`linux/measured-current-binary.json`|测速后观察到的二进制摘要；不是追溯补写逐轮校验|
|`windows-release.json`、`windows-debug.json`|最终Windows源码构建及各5项契约|
|`windows-real-proxy-model.json`、`windows-final-proxy-browser.json`|真实TLS/WS/SSE/模型与浏览器链路|
|`proxy-*.json`|公平池化对照、原始断言失败、已撤回实验；旧新建TLS对照不称为纯代理开销|
|`*-ui.json`、`ui/`|浏览器报告与固定环境截图；跨系统不要求逐像素一致|
|`source-ci.json`、`frontend-sha256.json`|本地工程检查与最终前端LF规范化摘要|

`linux/linux-evidence-sha256.json`用于核对首次下载的94份Linux文件；后取回的最终构建/运行报告单列，不改写初次摘要。最终文件总清单见 `delivery-sha256.json`；它不包含自身，避免循环摘要。

报告没有云Key、连接密码、模型、用户私人资料或数据库备份。Linux原始位置为 `/home/pikachu/phase11-data/function3/validation/phase43-47`。云端真实Key、主观音色/模型质量、远程CI与生产发布仍不属于通过结论。
