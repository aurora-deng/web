# Envoy 网关与环境入口 · 业务说明

网关配置把站点、研途和已允许业务转发至明确的上游。网页入口尽量复用同源，无需用户记住多个服务端口。HTTP、WebSocket 和 SSE 采用适合各自生命周期的转发设置；供私人推理访问的上游按契约校验证书。

部署前分别检查各入口和上游直连、资源版本、登录 Cookie、升级协议、健康摘除与回退。Windows 回环地址的可用性依赖本机转发或已配置的客户端入口。

[配置文件](../../../config/gateway/envoy.json) · [网关部署和排查](../../observability/envoy-gateway.md) · [研途路由](../../ops/runtime/nebula-yantu-community.md)

[实操学习](learning.md) · [业务目录](../README.md)
