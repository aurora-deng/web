# 推理适配与测量

- `contract/ModelProvider`：请求、取消、唯一完成、可选 token-ID usage 与内容无关队列快照。
- `inprocess/InProcessOnnxModelProvider`：已有 ONNX Runtime GenAI 模型与 Adapter 的有界内嵌推理。
- `remote`：gRPC Worker / Client 与 Ollama；保留鉴权、TLS、超时、取消和回压边界。
- `resilience/ResilientModelProvider`：同 runtime 的熔断与无首片段时降级；不将两份回答拼接。
- `metrics`：有限字段和固定桶；token-ID 数不等于文本片段数，gRPC 本地 RPC 队列不冒充远端推理队列。

源目录迁移只调整位置与 include / CMake 引用，不改变协议。历史记录保留原路径。未经审批不启动、上传或切换正式模型。
