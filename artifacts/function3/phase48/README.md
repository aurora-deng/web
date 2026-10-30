# Phase48 验收证据

2026-10-30。功能回归通过，**性能门槛未通过**；见`summary.json`和`linux/final-performance-comparison.json`。完整解释见[Phase48](../../../docs/architecture/business/phase48.md)。

- Linux Release/Debug/ASan-LSan-UBSan各115项；`linux/release-ci.json`及`linux/checks/`保留原始记录。
- `source-ci.log`：28项前端逻辑、12项工程测试及配色/边界/资源检查。
- 浏览器替身：chat-ui/workspace-ui/tree-ui、四个原有回归日志；不是实际模型质量测试。`ui/`保存三尺寸深浅聊天和神树截图。
- Windows两种构建各5项契约；`windows-real-proxy.json`为真实TLS/WS/SSE、私人ONNX、迁移、Agent和本地语音回退。原生峰值17.133MiB不含浏览器，1次空闲GET安全恢复。
- Linux九组真实业务和独立Worker故障见`linux/real-integration-summary.json`、`linux/worker-isolation.json`。这些完整模型检查先于最后观测/线程顺序修正；修正后重跑三种115项矩阵、真实工作区和新指标，不虚称全部真实模型用例最后再跑一次。
- `linux/benchmarks-final/`对最初基线，`linux/benchmarks-previous-final/`对直接上一版，均保留最终失败。无final的两个目录是较早构建的过程记录，不能替代最终成绩。diagnostic目录仅定位同步WAL等待，不参与门槛。
- 最终资源/源码/迁移核对在`linux/final-runtime-smoke.json`；测速二进制与状态页归档构建摘要分别记录。`linux/final-owned-processes.json`确认测试端口关闭；原库和日常服务未改动。

`linux/evidence-sha256.json`核验Linux报告原始字节；根目录`delivery-sha256.json`覆盖本交付所有证据（自身除外）。本目录使用局部.gitattributes保留字节，避免跨平台换行破坏摘要。无数据库转储、模型、Key或登录凭据。
