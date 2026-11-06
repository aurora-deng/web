# 聊天背景：镜像樱花水岸

更新：2026-11-05。按用户参考图的左右镜像构图继续细化聊天背景：左侧弯曲树干与根部、左上粉白花冠、中央柳絮，右侧保留开阔天空和水面，下方加入柔和倒影与零星落花。

## 1. 实现和边界

- 用内置 imagegen 生成原创背景素材，构图参考上传图片与此前手绘。正式页面素材为 [sakura-lake-5d7c79f59e36.jpg](../../../../frontend/phase11/chat/ChatBackdrop/sakura-lake-5d7c79f59e36.jpg)，1672×941，325486 字节，约 318 KiB。
- JPEG 源素材文件名包含 SHA-256 前缀，完整摘要为 `5d7c79f59e362a5a7a792e9fee46d622567a08ecdd545b07d0be622cc5c06c62`。当前服务器只发布原生 HTML/CSS/JS，实际页面通过生成的 `ChatBackdropArtwork.js` 加载内嵌 JPEG，避免请求未注册的图片路由；模块传输约 424 KiB。资源内容进入统一版本摘要，更新后不会继续使用旧图。无需修改服务器程序。
- 背景在消息区固定，消息列表独立滚动，输入区仍贴底；布局沿用既有 Grid，不改通信或业务架构。
- 深色使用遮罩压低背景亮度，浅色保持粉白与湖水层次；发送者和时间标签使用独立淡底，消息气泡保留清晰表面。
- `pointer-events:none`、`inert`、`aria-hidden` 保证装饰不挡点击、文本选择或辅助阅读。图片懒加载、异步解码，预留尺寸避免布局跳动。
- 桌面固定 20 个粒子，手机显示 8 个；柳絮集中在中部，少量花瓣和叶子从左侧飘散。图片本身不逐帧重绘，不新开 WebGL 上下文；页面隐藏或不可见时暂停，减少动态设置下关闭粒子。
- 手机保留左侧树干和花冠，降低背景强度。此次不调整撤回、AI、数据库或协议规则，不安装依赖。

## 2. 文件索引

|职责|文件|
|---|---|
|同源图片与粒子标记、生命周期|[ChatBackdrop.js](../../../../frontend/phase11/chat/ChatBackdrop/ChatBackdrop.js)|
|生成的图片数据模块|[ChatBackdropArtwork.js](../../../../frontend/phase11/chat/ChatBackdrop/ChatBackdropArtwork.js)|
|从源JPEG生成模块及一致性检查|[chat_backdrop_artwork.py](../../../../scripts/project/chat_backdrop_artwork.py)|
|裁切、叠层、字幕淡底和动画|[ChatBackdrop.css](../../../../frontend/phase11/chat/ChatBackdrop/ChatBackdrop.css)|
|深浅主题参数|[tokens.css](../../../../frontend/phase11/shared/DesignSystem/tokens.css)|
|资源摘要、体积与组件契约|[chat_recall_backdrop.test.mjs](../../../../tests/function3/frontend/chat_recall_backdrop.test.mjs)|
|生成提示词与验收记录|[历史验收摘要](../../../ops/validation/function3.1.md#历史补丁)|

## 3. 验证与更新

真实发布前端包配合本机 HTTP/WS 替身检查：1440×900、1920×1080、390×844；深浅主题、图片实际解码、无横向溢出、输入框贴底、装饰不拦截发送与服务器确认后的消息状态。该场景使用合成数据，不向原数据库写测试账号和消息。

前端完整回归及工程门禁的结果见本轮报告；图片内容哈希和 512 KiB 体积上限纳入已有组件测试。此次纯页面更新不重新宣称服务器压力测试或后端 Sanitizer 覆盖。

发布包版本 `919e98a2ba67ff14`，已获得用户明确批准并上线18090，运行PID27634；服务就绪，原库68个账号及密码散列、程序二进制、Token、证书和迁移24均未改变。首次直接JPEG版本因图片路径404触发自动回退，记录保留；随后改为原生模块封装，在 Linux 隔离18094的真实资源路由上验证全部补丁文件返回200、MIME正确、解码后的JPEG摘要一致，浏览器也完成真实解码复验。

更新复用原18090服务、数据库与账号，保留之前页面副本，程序二进制和迁移版本不变。只有 `ChatBackdrop` 和主题 Token 的内容被替换，其余前端仅更新统一资源版本引用；本地其他未上线业务改动不混入该包。状态以[历史验收摘要](../../../ops/validation/function3.1.md#历史补丁)为准。

三分钟撤回规则与前一轮可靠性修复继续有效，详见[撤回说明](chat-recall-and-tree-background.md)。上线后访问[聊天页面](http://192.168.239.135:18090/phase11/?v=919e98a2ba67ff14)并刷新。
