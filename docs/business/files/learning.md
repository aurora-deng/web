# 文件、聊天附件与社区媒体 · 学习与复验

1. 在隔离站调用 `GET /api/client-capabilities`，查看聊天与社区用途的上传限制，再用合成图片测试允许格式、类型不匹配和超过限制三种结果。记录 HTTP 状态码。
2. 在 A 与 B 私聊上传，保存附件 URL。先用 B 下载，再用无权账号 C 下载。将消息撤回后再用 B 重试同一 URL，核对请求拒绝。
3. 跑 `ctest --test-dir "$WEB_BUILD" -R 'phase11_file_tests' --output-on-failure`。尝试用备份后的隔离数据恢复一条旧附件，同时核对记录和文件摘要。附件目录就像档案柜，关系和内容状态决定谁还能领到文件。

[业务复现与浏览器冒烟](../../ops/validation/reproduce.md)

[业务说明](README.md) · [业务目录](../README.md)
