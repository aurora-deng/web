# Phase35 证据索引

本目录对应原库复用、语音修复、Redis与前端改版。Phase34的原始结果仍保留在相邻 `function3/` 目录，没有被本轮结果覆盖。

## Linux 原始结果

| 文件 | 内容与边界 |
|---|---|
| `MANIFEST.json` | Linux相关源码整体摘要及白名单结果文件SHA-256，不包含模型、密钥或上传文件 |
| `release-ctest.txt` | 最终Release完整CTest |
| `debug-ctest.txt`、`debug-summary.json` | 最终Debug回归、配置与步骤耗时 |
| `asan-ctest.txt`、`asan-summary.json` | 项目代码ASan/LSan/UBSan回归；预编译第三方库未重新插桩 |
| `local-delivery.txt` | 已有隔离库中的HTTP→PG→本地持久投递→男女TTS，媒体、回复目标、权限、h2与取消 |
| `shared-runtime.json` | 原`nebula_owner`登录、原库、Redis命中、两种真实WAV时长/RMS/摘要；不发布业务内容 |
| `migration-report.json` | 原库68账号整体摘要不变、实际执行迁移及备份位置；无逐账号散列、密码或备份正文 |
| `read-cache-first-attempt.json` | 第一轮短突发负收益，不删除不利结果 |
| `read-cache-benchmark.json` | 最终4Worker×8秒、AB/BA/AB三轮开关缓存比较及不可达回源；不是全站容量 |

## Windows 浏览器与可移植检查

| 文件 | 内容 |
|---|---|
| `live-browser.json` | 实际VM页面登录、三页导航、真实播放器时间前进、390×844三页无横向溢出、无页面异常 |
| `tree-browser.txt` | 合成接口下真实WebGL、分视图、发布、回复目标、举报、10k有界DOM与后台权限 |
| `announcement-browser.txt` | 合成公告下原生音频、播放受限恢复、录音暂停/恢复、多标签去重与销毁 |
| `chat-browser.txt` | 历史分页、断线恢复、丢ACK、待确认恢复与角色入口五组 |
| `node-tests.txt` | 18项历史/幂等重试/恢复预算/窗口算法测试 |
| `architecture.txt` | 同名组件与业务/驱动依赖边界检查 |

界面预览使用**合成数据**，包括十万果实显示文案；它不表示原业务库拥有这些数据，也不单凭截图证明容量：

- [神树与统一导航](tree-overview.png)
- [评论详情布局](tree-comments.png)
- [移动视图](tree-mobile.png)

实际原库验收只进行登录、查询和固定文案试听；Session/审计/最后活跃时间可能更新，没有写入测试动态、评论、合成账号或聊天正文。破坏性夹具仅在既有`function3_runtime_test`及契约测试库中执行。

复现入口与解释见 [Phase35](../../architecture/business/phase35.md)、[日常启动](../../ops/function3-runtime.md)。首次负收益、Phase34的ACK P99失败均保留；不能用公共分类接口的提升覆盖聊天写入的未达标结论。
