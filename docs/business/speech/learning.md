# 公告、通知偏好与语音 · 学习与复验

1. 在隔离站产生一条榜单变化公告，打开两个标签页，先看文字出现，再打开用户声音并等待音频。把字条和扬声器当成两个独立通道，分别记录文字到达、音频就绪和播放时间。
2. 用同一试听文案切换可用音色、语速和音调。用测试配置模拟 Provider 超时或未配置，分别检查试听报错、回退选择及文字可读性。
3. 跑 `ctest --test-dir "$WEB_BUILD" -R 'function3_speech_link_contract|function3_speech_policy' --output-on-failure`，并对照实际听音记录。

[语音接入讲义](../../architecture/business/guides/speech-integration.md) · [可配置的语音路径](../../ops/runtime/free-speech.md)

[业务说明](README.md) · [业务目录](../README.md)
