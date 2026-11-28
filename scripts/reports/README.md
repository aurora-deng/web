# 测试报告与输出适配

- `gen_report.sh`：编排构建、单元测试、黑盒、压测和报告。`--help` 在运行阶段前退出；正常执行可能启动服务或清理测试端口，必须确认目标环境。脚本内生成的新报告指向规范入口，不改写已有历史报告。
- `wrk_report.lua`：将 wrk/wrk2 统计转换为既有 `KEY=VALUE` 格式，不改变百分位回退与错误累计方式。

示例：`bash scripts/reports/gen_report.sh --help`。旧 `scripts/gen_report.sh` 保留薄转发；旧 `scripts/wrk_report.lua` 根据自身文件位置加载本目录的唯一实现，不依赖调用者当前目录。

[脚本总览](../README.md) · [基准驱动](../benchmarks/README.md)
