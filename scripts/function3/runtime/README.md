# 日常运行控制

`service.py` 保持原默认启动方式；新增 `--ai-worker` 为显式可选模式，不自动切换模型版本。
在批准部署并完成模型注册后，`start --ai-worker --ai-worker-cpus 2,3` 会先启动 loopback TLS/token Worker，再启动主服务。
CPU 集合必须属于允许亲和性；只给 Worker 使用 taskset 与 nice，不改主服务优先级，不安装系统软件。
Worker 仅继承运行路径 / locale，拒绝继承数据库连接、代理或云密钥；TLS 使用已有证书，不关闭校验。
关闭顺序为主服务、随后已核实可执行文件 / start ticks 的日常 Worker；不强杀、不控制 validation Worker 或其它端口。

启动失败只回收本次创建的进程；主服务无法优雅退出时保留 Worker，交给操作者检查。
TLS 握手只表示传输可用，不代表模型已经加载、RPC token 已通过远端鉴权或数据库绑定正确。
脚本在源码层完成离线合成契约，不代表本轮已经在正式服务开启。运行 start/stop、迁移和部署仍需当前审批。
