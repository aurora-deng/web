# ai 模块

职责：需要聊天上下文、权限和审计的 AI 业务编排。

输入来自该模块的 `ports/`、`transport/` 或 `bootstrap/`；输出为生成状态、知识引用与训练候选。业务实现放 `application/`，模型放 `domain/`，驱动放 `infrastructure/`。没有对应职责时不创建空层。

采用根项目现有 C++ 标准、CMake 目标和依赖版本，本轮不新增依赖、不改变认证和事务语义。底层推理和模型训练仍在 backend/ai/，不在这里重复实现。

验证：`python tests/project/test_application_layout.py`；完整源码检查：`python scripts/project/ci_checks.py source`。这些检查不代表正式部署或性能验收。

规范与限制见 [业务目录规范](../../../../docs/architecture/overview/application-domain-layout.md)。
