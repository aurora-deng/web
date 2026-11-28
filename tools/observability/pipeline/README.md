# 观测依赖供应

`install_metrics_backend.py` 与 `install_pipeline.py` 按既有固定版本、摘要、许可和预算供应 Linux 观测依赖。输入是显式选择的供应参数与经过审批的包；输出在原隔离依赖目录，不是全局安装位置。

兼容入口如 `python -m tools.observability.install_pipeline --help` 只显示帮助。模块导入不下载、不解包、不执行二进制；实际供应有下载、文件写入和程序执行影响，必须先审批。此次重构没有运行安装器，也没有修改其固定版本、下载地址、摘要或许可校验。

[返回工具目录](../README.md)
