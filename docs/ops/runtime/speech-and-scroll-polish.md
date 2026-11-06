# 语音切换试听与柔和滚轮切页

更新：2026-11-05。延续已上线语音修复，通信核心、数据库结构与Key保存范围不变。本轮不安装依赖、不创建云资源、不使用第三方客户端签名材料。

后续[免绑卡语音替换](free-speech.md)将底部按钮改为“试听当前音色”，加入设备中文目录与可取消朗读，删除旧代理。本文其余滚轮和历史验收范围保持不变。

## 使用与测试

1. 顶部“语音设置”打开窗口。窗口只有表单正文滚动，关闭按钮、配置反馈、“连接并试听当前音色”保持可见。
2. 先选择不需要Key的本地Kokoro，分别选择男女声并点击试听，生成后可暂停、停止或手动播放。
3. 更换音色、语速等设置后，旧试听立即清除。底部显示当前目标；再次点击试听生成新音频，不会把上一段音频当成新音色。
4. Azure：展开“第一次使用：获取并填写Azure Key”。在自己的Speech资源“密钥和终结点”复制KEY1或KEY2，填写对应资源区域；确认套餐，获取可用音色，再试听。Key只输入页面，不发到聊天；保存后输入框清空，以“已配置/未配置”状态为准。
5. 个人配置和站点公告仍分开；个人试听不能改写全站声音。真实云测试需要用户自己的Key，目录或替身成功不能当成真实合成通过。

## 滚轮与阅读

- 页面底部继续滚动时先出现进度提示，再通过约300毫秒的轻微位移和淡入淡出切页。单个大滚轮事件不会直接跳走；一段连续触摸板惯性不连跳两页。
- 长故事列表先正常滚动；刚滚到边界的惯性不立即切页。上翻回到上一页底部，点击侧边栏返回时恢复该页阅读位置。
- PageDown/PageUp保留明确切页行为。尊重系统“减少动态效果”设置；弹窗、表单、播放器和助手区域不触发切页。
- 3D画布内的触摸仍用于转动视角；画布外竖向滑动可以切页。普通滚轮不改变缩放，Alt＋滚轮和缩放按钮保持可用。

## 组件边界

|组件|职责|
|---|---|
|[TreePageNavigation](../../../frontend/phase11/tree/TreePageNavigation/TreePageNavigation.js)|输入方向、边界、手势预算、惯性/重复导航限制与进度通知|
|[TreePageTransition](../../../frontend/phase11/tree/TreePageTransition/TreePageTransition.js)|页面动画、最新导航优先、阅读位置、减少动态效果与销毁|
|[组件样式](../../../frontend/phase11/tree/TreePageTransition/TreePageTransition.css)|轻量提示条，不增加3D后处理或逐帧GPU读取|
|[TreeApp](../../../frontend/phase11/tree/TreeApp/TreeApp.js)|连接既有页面状态与以上组件；不在切页时重复读取业务数据|
|[SpeechSettings](../../../frontend/phase11/shared/SpeechSettings/SpeechSettings.js)|固定操作栏、Key引导、当前音色反馈、切换后清理旧播放器|
|[CloudSpeech](../../../frontend/phase11/speech/WebSpeechApi/CloudSpeech.js)|自选音色不继承另一个男女预置不支持的表达方式|

[历史验收摘要](../validation/function3.1.md#历史补丁)区分浏览器界面夹具、真实服务资源核对和待Key配置后的云试听。保留此前真实本地TTS与HTTP/2音频证据；本轮未重新执行全服务器Release/Debug/Sanitizer矩阵，不宣称QPS改善。
