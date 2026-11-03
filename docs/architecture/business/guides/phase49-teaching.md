# Phase49 教学：接口怎样对齐，模块怎样独立而又能协作

## 1. 对齐的是协议合同

前端与后端像点餐端和厨房：需要同一份菜单编号、份数规则和出餐回执，不要求点餐屏的每个按钮都对应厨房的一间房。

- 前端页面负责交互、加载/错误状态、取消与渲染。
- 领域 API 绑定负责请求方法、路径和参数占位符。
- HTTP / WS 适配器负责认证、协议输入解析和输出序列化。
- Service 负责权限、状态、事务和幂等。
- Repository Port 是业务需要的数据能力，驱动实现 SQL 或内存读写。

因此对应关系是“一个明确用例 → 一个或多个已定义契约”，文件按领域归类。一次发送可以随后收到 ACK、送达和已读事件；把它强行缩成一个按钮对应一个同步接口，反而丢失真实生命周期。

## 2. 找用户为什么归到 contacts

从 `frontend/phase11/contacts/ContactsApi/ContactsApi.js` 的 `getUsersSearch` 出发，跟踪：

```text
UserPicker / ContactsController
  → ContactsApi.getUsersSearch
  → BusinessApi.request
  → GET /api/users/search
  → UserDirectoryHttp：认证、字段解析
  → UserDirectoryService：账号、查询与分页规则
  → IUserDirectoryRepository
  → PostgresStore：绑定当前连接
  → PostgresWorkspaceRepository：SQL、行解码与公开投影
  → UserPage → JSON → 用户选择器
```

前端绑定由 `scripts/project/sync_api_contracts.py` 从实际业务路由生成，入口索引为 [business-routes.json](../../../contracts/business-routes.json)。检查模式会拒绝过期绑定、失效成员名、缺少导入或缺失模块。`BusinessApi` 在发送之前检查路径、方法和领域描述是否匹配，拼错路径不再等到线上 404 才发现。

这层检查不能证明 JSON 字段语义或权限正确。请求/返回字段继续由 OpenAPI、实时契约和真实接口测试约束；后端每次仍校验权限。Windows `/api/local/*` 和用户直接连接的外部模型 API 有各自边界，不混入 Linux 路由索引。

## 3. Query、数据库行、公开结果为什么分开

```cpp
// 传条件，不传数据库连接；actor 由认证层提供。
UserDirectoryQuery input{"小明", 0, 20};
UserPage page = directory.search(authenticatedUser, input);
```

Query 是“我要找什么”，DAO 行是“数据库查出了什么”，返回投影是“允许用户看到什么”。比如数据库里的用户还有密码散列，但用户搜索只返回最小公开资料；头像 ID 会由 HTTP 层转成页面 URL。完整规范、真实代码和事务图见 [数据库领域分层](../../../database/domain-layering.md)。

## 4. main 简短之后，由谁真正负责生命周期

`main.cpp` 只创建 `ApplicationRuntime` 并处理启动异常。`ApplicationRuntime` 连接通信、运维、业务与 gRPC；`BusinessRuntime` 按依赖顺序组合各领域 Runtime。它像总装车间的工序表，各 Runtime 才是负责自己零部件的工位。

`unique_ptr` 本身没有错；它明确一个资源只有一个主人。问题在于入口亲自管理几十个不相干资源，还需要人工猜测销毁顺序。现在资源主人回到相应领域：数据库连接池和执行器在 `StorageRuntime`，认证在 `IdentityRuntime`，AI Provider/任务在 `AiRuntime`，实时命令和 Outbox 在 `RealtimeRuntime`。

停止顺序明确为：网络任务排空 → gRPC 停止 → 解除 Presence 回调 → 取消私人任务 → AI 停止 → 文件与知识任务排空 → 数据库任务排空 → Outbox 停止 → 社区后台任务停止。随后才按反向依赖销毁对象。重复 stop 安全；启动中途失败也由 RAII 释放已成功构造的组件。

## 5. 一个事务可以联系几个独立模块

会话免打扰属于聊天偏好用例：

```text
PATCH /api/conversations/{id}/preferences
  → 认证与 CSRF
  → SetConversationPreference
  → ChatWorkspaceService 开启事务
  → 校验当前成员资格
  → 写当前用户的偏好和版本
  → 有变化时写 conversation.preferences Outbox
  → 一起提交，再唤醒投递
  → 其他设备根据版本收敛
```

偏好只控制提醒，不停止消息保存、恢复或未读计数。Repository 被分成几份，也必须使用同一条事务连接；这是模块“相对独立又互相联系”的关键。

## 6. 怎样判断这次重构有没有完成

- 看归属：服务、适配器和前端控制器是否能在所属领域找到。
- 看接口：路径/方法之外，字段、权限、错误与分页是否还有真实测试。
- 看退出：半途构造失败、TLS 错误、监听占用、任务取消是否安全。
- 看兼容：旧头文件仅转发、旧方法仅委托，没有复制新旧两份规则。
- 看证据：源码检查、浏览器协议替身、真实 PG/网络、真实模型与性能分开记录。

建议先完整跟踪“搜索用户”，再跟踪“修改免打扰”，最后跟踪“消息+Outbox”。先理解每条链路的输入、事务和返回，再继续研究更复杂的 AI 编排。
