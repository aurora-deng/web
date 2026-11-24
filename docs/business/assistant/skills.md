# 小研技能 · 使用与实现

登录 NEBULA，打开小研的独立对话页或侧栏，展开「小研技能」。网页首次生成前确认模型的发送范围。Windows 私人空间使用「设置」里选定的私人模型。

## 用一次考研周计划上手

1. 选择「考研周计划」，输入「408 优先计网，工作日每天 2 小时，周末 4 小时」。
2. 需要结合账号里的任务负担时，勾选「参考研途任务数量」。小研只取当天任务、当天完成、逾期未完成和接下来七天未完成四项计数。统计日期按浏览器的本地日期确定。
3. 点「生成草稿」，核对七天的任务量和完成标准。可以直接改草稿，再复制到研途。确认可完成的任务后，在研途自行添加。

## 技能目录

| 技能 | 适用场景 | 实际交接 |
|---|---|---|
| [考研周计划](../../../skills/kaoyan-week-plan/SKILL.md) | 拆分科目目标、限时练习和复盘 | 复制计划或打开研途 |
| [错题复盘](../../../skills/kaoyan-review/SKILL.md) | 梳理错因、重做步骤和自测题 | 复制清单或打开研途 |
| [聊天起草](../../../skills/chat-draft/SKILL.md) | 帮你把约时间、协作和沟通需求写成一条消息 | 点「读取选项」，选会话，带入聊天框后发送 |
| [广场动态](../../../skills/plaza-draft/SKILL.md) | 将自己提供的经历写成一段动态 | 选分类，打开广场编辑器，确认可见范围后发布 |

聊天草稿绑定所选会话，切到另一个会话时需清空或切回原会话。广场预填保留原发布器的可见范围确认、CSRF 和幂等键。四个技能都允许直接复制和编辑。模型质量随所选模型变化，复核错题答案和动态里的事实后再使用。

## Tool 怎样接入

Skill 像一张工作步骤卡，Tool 是一步可以调用的查询。以考研为例，步骤卡给出每天最多三件事的规则，查任务的 Tool 提供四个数字，模型据此建议本周任务量。查询工具只在用户勾选或点击后执行。模型输出进入文本编辑框。

| 只读 Tool | 实现与参数 | 返回 |
|---|---|---|
| `study.read_progress` | 用当前会话读取 `/api/study/session`，再请求工作区下的 `GET api/assistant-summary?day=YYYY-MM-DD` | 日期与四项计数。服务端排除标记为本地来源的资料及无效任务 |
| `chat.list_conversations` | 查询本人 `GET /api/conversations` | 会话 ID、标题和会话类型，用于手选目标 |
| `plaza.list_categories` | 查询 `GET /api/tree/categories` | 分类 ID 与名称，用于手选目标 |

前端在生成、读取选项及交接时校验当前账号。模型收到本次说明和明确勾选的研途统计，聊天、广场选项在生成后单独加载。交接文字存于当前标签页，十分钟后失效，URL 只携带随机编号。成功带入后清掉临时记录。编辑框已有内容时保留现场。研途账号入口、现有模型和发布器均沿用现有部署方式。 [研途账号运行手册](../../ops/runtime/nebula-yantu-community.md) · [接口契约](../../contracts/README.md)

### 扩展一个技能

在 [SkillCatalog](../../../frontend/phase11/shared/AssistantSkills/SkillCatalog.js) 登记用途和提示，按需在 [SkillTools](../../../frontend/phase11/shared/AssistantSkills/SkillTools.js) 注册一项窄用途查询。程序按允许的字段读取工具结果，新增外部动作继续走对应业务页面的确认。根目录的 `skills/<id>/SKILL.md` 用于阅读、演练和复用步骤。网页按目录代码加载。 [Agent Skills 规范](https://agentskills.io/specification)

### 本地复验

```powershell
node --test tests/function3/frontend/assistant_skills.test.mjs
python -B tests/client/yantu_account_workspace_test.py
node tests/function3/frontend/assistant_skills_browser.cjs
```

浏览器回归复用已安装的 Playwright 和 Chromium 系浏览器，可用 `PLAYWRIGHT_MODULE`、`CHAT_TEST_BROWSER` 指定位置。账号和模型数据由测试夹具提供。研途 HTTP 合约会启动临时账号服务，需要本机回环权限。

[小研业务说明](README.md) · [版本验收](../../releases/v0.5-agent-skills.md)
