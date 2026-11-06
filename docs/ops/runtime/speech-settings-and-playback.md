# 语音配置、试听与神树滚动：使用和修复说明

更新：2026-11-05。本文记录Phase49之后的语音与交互修复，不把Phase50性能方案标为完成。通信核心与原业务库结构保持不变；没有新增数据库、模型或依赖。

最新使用方式见[免绑卡语音](free-speech.md)：默认复用Kokoro，网页版新增实际设备中文音色、直接朗读和停止；停用的兼容代理及专用转码已移除。Azure/Gemini是已有Key用户自选能力，正常使用无需开通云账号。

## 1. 在哪里设置

所有主页面顶部新增 **“♫ 语音设置”**。原“声音与通知”菜单仍负责开启全服公告声音、选择公告男女声和试听。

|设置范围|入口|保存什么|影响什么|
|---|---|---|---|
|普通网页版个人语音|顶部语音设置|浏览器按账号保存非敏感偏好；Key 只在当前页面内存，刷新/离开后清除|个人 API 连接测试与试听，不修改全站公告|
|Windows 个人语音|同一入口，从本机客户端打开|沿用账号隔离 SQLite 与 DPAPI 凭据保护|本机个人音色、API 和试听|
|全站公告|管理中心 → 公告音色 → 设置云语音、表达与回退|音色设置在原库；Key 在 Linux 私有文件|后续全服公告及管理员试听|

站点管理员才能更新全站 Key；群主或内容管理员不自动拥有这个权限。个人设置和站点设置分开，不能把保存了个人试听音色误认为已经修改全站广播。

## 2. 如何配置和测试

1. 本地 Kokoro 不需要 Key。选择男女声、调整语速，点击“试听当前音色”。首次合成可能需要一两分钟。网页版还可选择“浏览器设备朗读”，获取实际中文目录后直接试听；设备没有对应音色时明确提示，不自动安装或转用收费服务。
2. Azure：选择 Azure Speech，输入资源对应区域和自己的 Key，确认套餐与额度后测试。网页版还可填写明确的云音色名称；留空使用晓墨/云希预置。自定义 Azure 音色使用自然表达，避免套用不支持的情绪。
3. Gemini：选择 Gemini TTS，输入自己的 Key，确认套餐的数据用途后测试。当前默认适配 Gemini 3.1 Flash TTS Preview 和固定官方接口，个人设置可显式选择旧2.5兼容；音色默认 Kore/Charon，网页版可指定已支持的预置音色名称。
4. 生成后显示真正的播放器，以及独立的“播放声音”“停止播放”。若浏览器拦住自动播放，再点一次播放即可；不能把打开公告声音当成启动服务器 TTS 引擎。
5. 网页个人连接测试直接调用所选云服务的固定 HTTPS 地址，Key 不进入 Linux 和浏览器持久存储。网络、跨域、Key 或额度错误会明确报错，不把失败伪装成连接成功。

站点与 Windows 路径继续使用既有云优先/本地回退策略；页面显示实际 Provider 和回退原因。本地生成成功只能证明本地语音可用，不能证明云 Key 正确。应用计数也不是云平台实际余额。

参考协议：[Azure REST TTS](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/rest-text-to-speech)、[Gemini TTS](https://ai.google.dev/gemini-api/docs/speech-generation)。截至本次核对，官方[停用计划](https://ai.google.dev/gemini-api/docs/deprecations)列出旧2.5 Flash Preview TTS将于2026-11-17停用；[价格页](https://ai.google.dev/gemini-api/docs/pricing)仍列出3.1 Preview的免费档。3.1也属于预览版本，真实可用性和额度需用自己的Key测试，不自动切换到付费模型。可用模型、音色和套餐以服务商实际返回为准。

## 3. 全站 Key 的保存与权限

新增 `PATCH /api/admin/speech/credentials`，使用现有 Session、CSRF 和站点管理员鉴权，并要求实际 HTTPS 连接。普通 LAN HTTP 页面会在提交前阻止发送站点 Key；Windows 本机代理仍须使用已经验证的 Linux HTTPS 链路。

默认文件位于 `/home/pikachu/phase11-data/function3/speech-secrets/azure.key` 和 `gemini.key`，目录0700、文件0600；也兼容已有 `WEB_SPEECH_AZURE_KEY_FILE`、`WEB_SPEECH_GEMINI_KEY_FILE` 指定的私有文件。文件路径不由网页请求指定。

写入使用临时私有文件、同步和原子替换，拒绝符号链接、FIFO、公开可读文件及非当前用户所有的文件。Key 不回显、不放入 SQL 或审计正文；审计记录申请与保存操作。更新 Key 无需重启；改变环境变量中的路径才需要重新启动。不要提交这些文件。

站点试听不再复用已有云音频缓存来冒充一次新的连接测试。普通公告仍使用有界音频缓存。

## 4. 为什么原来可能只有文字或者试听无声

- 网页个人设置原先在没有 Windows 客户端时直接返回提示，表单实际无法打开。
- TTS 异步完成时，最初点击产生的浏览器播放许可可能已经过期；旧播放器藏在菜单中，缺少清楚的手动恢复入口。
- 旧手动续播同时检查“公告声音已开启”，导致关闭自动播报时连主动试听也不能恢复。
- 原音频接口使用 HTTP/1 文件响应；冻结的 HTTP/2 适配不支持对应 sendfile 路径。现在短语音在业务适配层校验为最多一分钟、6 MiB 的单声道 PCM WAV，再使用有限内存响应返回，保留 MIME、无缓存与鉴权。HTTP/2 原有单次响应上限仍为1 MiB，较长音频通过每段最多512 KiB的Range响应下载，前端严格检查连续区间和总大小后重组成WAV；不提高通信核心上限。超过上限却不提供有界Range的HTTP/2请求会明确拒绝，不会发出不完整音频或没有Range请求的206响应。
- 公告先下载并校验音频，再领取一次性播放权，避免一次网络加载失败就消耗播放权；合成、HTTP错误、无效WAV与播放许可分别给出状态。

文字公告仍独立于语音；不通过关闭持久化、权限或背压换取播放效果。

## 5. 神树如何滚动切页

顺序为 **神树全景 → 故事广场 → 七日心动榜**。到当前页面底部后继续向下滚动进入下一页；到顶部继续向上可返回。长故事列表先正常滚动，不会一滚轮就跳走。也支持 PageDown/PageUp 和手机竖向滑动。

最新[交互优化](speech-and-scroll-polish.md)：边界处显示切页进度，约300ms轻微位移/淡入淡出；等待旧惯性结束，避免同一手势连跳。返回时保留阅读位置，系统减少动态效果设置生效。语音设置操作栏固定可见，更换音色后必须生成新试听。

普通滚轮用于阅读/切页；需要缩放神树时使用 **Alt＋滚轮** 或新加的放大/缩小按钮。拖动旋转与点击果实保持可用。弹窗、输入框、导航、播放器与助手侧栏不触发切页；连续触摸板事件有切页间隔，避免直接连跳两页。

## 6. 组件与验证入口

- [语音设置](../../../frontend/phase11/shared/SpeechSettings/SpeechSettings.js)、[网页个人语音适配](../../../frontend/phase11/speech/WebSpeechApi/WebSpeechApi.js)、[固定云协议](../../../frontend/phase11/speech/WebSpeechApi/CloudSpeech.js)。
- [音频校验与播放](../../../frontend/phase11/shared/SpeechPlayback/SpeechPlayback.js)、[公告播放器](../../../frontend/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.js)。
- [站点接口](../../../backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.cpp)、[有界音频响应](../../../backend/ai/speech/transport/SpeechHttp/AudioResponse.h)、[私有文件驱动](../../../backend/ai/speech/infrastructure/SpeechCredentialStore/SpeechCredentialStore.cpp)。
- [滚动切页组件](../../../frontend/phase11/tree/TreePageNavigation/TreePageNavigation.js)、[神树控制](../../../frontend/phase11/tree/TreeScene/TreeScene.js)。
- [前端契约测试](../../../tests/function3/frontend/speech_navigation.test.mjs)、[真实 Linux 验证](../../../tests/function3/integration/speech_playback_blackbox.py)、[凭据文件测试](../../../tests/function3/unit/speech_credentials.cpp)。

验证须区分：替身云响应、真实本地合成、浏览器实际解码播放、真实云服务和听感。没有提供真实云 Key 时，不宣称云调用或自然度已经通过。日常服务的部署状态单独记录，源码修改不等于18090已更新。

2026-11-05收尾：[历史验收摘要](../validation/function3.1.md#历史补丁)。44项前端、Windows Release 5项契约及Linux组件/接口检查通过；真实男女声HTTP/1与H2一致，约1.5 MiB分段重组完整，浏览器31.6秒样本解码/手动播放和停止通过。日常部署与真实云Key仍单独确认。后续见[工程计划](../../roadmap/engineering-credibility.md)。

2026-11-05补查：[历史验收摘要](../validation/function3.1.md#历史补丁)。修复生成/下载中切换音色仍播放旧结果的竞态；自动播放受限时等待手动操作，录音、过期、关闭和完成状态不再静默。当时前端51项、真实排名公告与男女声合成、并发单次领取及Windows个人语音/音频Range代理通过。

2026-11-05上线：[历史验收摘要](../validation/function3.1.md#历史补丁)。明确批准后恢复18090，初次资源`511c4931f8cde9f2`，原库68个账号不变；真实普通用户试听4.54秒播放到结尾。VM内部TLS正常，Windows直连18443仍超时，未修改防火墙。随后Azure目录与[试听/滚轮交互增量](speech-and-scroll-polish.md)上线，当前资源`5ec4d82e27389fab`；真实Azure调用仍需要自己的Key。
