# 研途账号工作区与跨端同步 · 业务说明

研途由独立 Python AccountWorkspace 提供。网页和客户端使用同一个 NEBULA 账号验证身份，再读取对应工作区、任务、学习会话与附件。相关 API 位于 `/api/study/*`，客户端支持退出和切换账号。原 Windows 独立研途资料与账号同步工作区分别存储。

服务器端维护工作区版本和访问控制，冲突提交按版本处理。上线需同时检查网关路由、会话校验、两个客户端的版本冲突、附件访问范围和工作区恢复。现有 Windows 独立资料继续通过用户原有路径使用。

[Python 工作区](../../../backend/study/AccountWorkspace/AccountWorkspace.py) · [部署和回退](../../ops/runtime/nebula-yantu-community.md)

[实操学习](learning.md) · [业务目录](../README.md)
