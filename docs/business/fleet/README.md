# gRPC 车队调度、边缘接收与对账 · 业务说明

协调器在 `backend/fleet/` 接收工单并按机器人能力、站点和队列状态派发。边缘确认工单后持久化接收状态，再执行后端 DAG 并回报结果。协调器与边缘各有 SQLite 账本。工单编号和派发代号用于重复请求、旧连接回报和重启对账。

Web 与 Fleet 采用独立配置、端口和进程。现有跨机实验的 gRPC 经过 SSH 转发。生产推广前要核对部署的传输保护、身份凭据轮换、负载流配额、完整 gRPC TSan 和实际尾延迟目标。ROS 导航在仿真接口验证阶段。

[协调器](../../../backend/fleet/application/FleetCoordinator/FleetCoordinator.cpp) · [边缘执行](../../../backend/fleet/application/EdgeWorker/EdgeWorker.cpp) · [gRPC 合同](../../../backend/fleet/transport/grpc/proto/fleet.proto) · [双机隔离](../../ops/validation/cpp-systems-cross-vm.md)

[实操学习](learning.md) · [业务目录](../README.md)
