# messaging 模块

职责：Outbox、投递编排和消息代理适配。

输入来自该模块的 `ports/`、`transport/` 或 `bootstrap/`；输出为可重试且幂等的投递结果。业务实现放 `application/`，模型放 `domain/`，驱动放 `infrastructure/`。没有对应职责时不创建空层。

采用根项目现有 C++ 标准、CMake 目标和依赖版本，本轮不新增依赖、不改变认证和事务语义。

验证：`python tests/project/test_application_layout.py`；完整源码检查：`python scripts/project/ci_checks.py source`。这些检查不代表正式部署或性能验收。

规范与限制见 [业务目录规范](../../../../docs/architecture/overview/application-domain-layout.md)。
