#pragma once
#ifndef WEBSOCKET_LIMITS_H
#define WEBSOCKET_LIMITS_H

#include <cstddef>

// 帧上限约束一次 wire frame；消息上限约束分片重组后的完整业务消息。
// 当前两者都取 1 MiB，但保留为两个独立概念，后续可以分别调节。
inline constexpr std::size_t kWsMaxFramePayloadBytes = 1024UL * 1024UL;
inline constexpr std::size_t kWsMaxMessageBytes = 1024UL * 1024UL;
inline constexpr std::size_t kWsMaxControlPayloadBytes = 125UL;

#endif
