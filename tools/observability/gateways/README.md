# 网关适配

`gateway.py` 负责读取项目中的 Envoy 模板、校验显式端口与 TLS 配置、渲染配置及管理隔离网关的进程生命周期。输入是模板、调用方参数与调用方提供的运行目录；输出是配置、受管进程或有分类的错误。

依赖 Python 标准库；实际启动仍需要调用方已供应并审批的 Envoy。模块导入不会启动进程。新代码引用 `tools.observability.gateways.gateway`，旧 `tools.observability.gateway` 保持同一模块身份。不得关闭证书验证、占用正式端口或为整理目录改变原限制。

[返回工具目录](../README.md)
