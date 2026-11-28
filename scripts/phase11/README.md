# Phase 11 兼容入口

本目录保留旧命令的薄转发入口，实际实现按职责归入 Function3 与 AI 目录。

- `scripts/function3/database/`：PostgreSQL 备份、迁移和用户级服务管理。
- `scripts/function3/data/`：演示数据初始化。
- `scripts/function3/runtime/webserver/`：Web 服务启动、停止与监督。
- `scripts/function3/validation/phase16_soak.py`：隔离环境稳定性验证。
- `scripts/ai/`：数据生成、训练、导出与工具链。

旧入口只转发参数，不维护第二份实现。CMake 和日常运行脚本使用新的职责路径。执行前阅读对应脚本的帮助与安全要求；迁移本身不会启动服务、修改数据库或安装依赖。
