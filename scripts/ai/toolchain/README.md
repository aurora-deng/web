# 批准工具链与固定模型来源

`install-lora-toolchain.sh`、`lora-constraints.txt`、`lora-requirements.lock.txt` 保存既有固定版本流程。
历史 `scripts/phase11/install-lora-toolchain.sh` 仅转发；旧约束 / lock 文件保留字节相同的兼容副本。
任何安装必须先给出版本、许可、位置、空间预算、影响和卸载方式并取得当前批准；脚本的 `--install` 不是代替人工审批。

`download_qwen_adapter_model.py` 只定义原有固定 Qwen 来源和完整文件校验。移动没有新增模型来源或下载权限。
`olive_oga017_compat.py` 在当前进程内适配 Olive 0.13 与 ORT GenAI 0.17 的加载名称，然后委托原 CLI，不修改全局依赖。
输入为现有批准环境 / 模型清单；输出按各 CLI 生成报告或下载文件。未批准时只读代码，不运行下载或安装。
迁移本身不运行安装器或下载器。2026-11-27 经明确批准，使用已有工具链完成真实 Olive 导出与 C++ 烟测，未安装 / 升级依赖或下载模型；失败及复测记录见 [优化进度](../../../docs/function3/optimization-progress.md)。迁移对应关系见 `docs/architecture/source-ai-script-migration.json`。
