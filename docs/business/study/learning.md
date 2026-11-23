# 研途账号工作区与跨端同步 · 学习与复验

1. 在项目根运行 `python tests/client/yantu_account_workspace_test.py`。该实验使用临时身份源和工作区，覆盖两个客户端、不同作者和附件权限。
2. 用测试账号分别从浏览器和研途客户端登录，建一条带有固定编号的合成任务。刷新另一端验证同步，再用旧版本提交，观察冲突状态。
3. 客户端切到第二个测试账号，尝试访问第一账号的任务和附件链接，记录拒绝。对照 `AccountWorkspace.py` 追到会话核验、工作区和附件授权。仅使用可丢弃的夹具。

[研途入口、联调及回退](../../ops/runtime/nebula-yantu-community.md)

[业务说明](README.md) · [业务目录](../README.md)
