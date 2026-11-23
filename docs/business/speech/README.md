# 公告、通知偏好与语音 · 业务说明

全站公告由业务事件生成，文字和音频状态分别发布。用户通过 `/api/me/notification-settings` 设置个人提醒，通过语音设置选择声音和试听。公告站点配置由有权管理员操作。个人语音可以按客户端环境选择本地或已配置 Provider。

公告语音任务和缓存可限额。浏览器需要用户授权播放；音频失败时仍可读取文字公告。上线时分别核对授权播放、真实声音、Provider 失败回退、音频访问权限和缓存清理，避免仅以成功产生字节判定听感和可用性。

[公告服务](../../../backend/application/modules/announcement/application/AnnouncementService/AnnouncementService.cpp) · [语音服务](../../../backend/ai/speech/application/SpeechService/SpeechService.cpp) · [语音设置与播放手册](../../ops/runtime/speech-settings-and-playback.md)

[实操学习](learning.md) · [业务目录](../README.md)
