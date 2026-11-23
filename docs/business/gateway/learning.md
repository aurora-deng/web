# Envoy 网关与环境入口 · 学习与复验

1. 用已批准的 Envoy 和隔离工作目录读一遍网关配置，画出站点、研途、静态资源与 WebSocket 四条转发路径。只用手册的测试端口。
2. 从同一浏览器比较上游直连和网关入口的 `/health/version` 与 `/phase11/asset-manifest.json`。切到研途做登录验证，再让一个隔离上游不可用，比较两段请求响应。
3. 跑 `python tests/project/test_gateway.py`。根据一个路由测试定位前缀顺序，再按手册检查长连接、摘除和回退后的实际结果。

[网关联调实验](../../observability/envoy-gateway.md)

[业务说明](README.md) · [业务目录](../README.md)
