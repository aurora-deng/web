# 按领域组织的业务模块

按“业务领域 → domain / ports / application / infrastructure / transport / bootstrap → 同名组件”定位代码。领域内按实际职责建层，不造空目录。顶层运行时和共享存储不为形式统一而迁走。

账号、联系人、聊天、文件、消息投递和 AI 的类型、接口、协议适配已分别归位；既有社区模块继续沿用同一规则。聚合头只在 `shared/compatibility/` 用于兼容装配。

输入、输出、验证命令和限制见各领域 README；[全量规范与迁移边界](../../../docs/architecture/overview/application-domain-layout.md)。
