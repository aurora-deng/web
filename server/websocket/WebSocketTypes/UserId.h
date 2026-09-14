// =============================================================================
// 文件名：UserId.h
// 所属模块：server/websocket/WebSocketTypes —— WebSocket 用户标识类型定义
//
// 【职责比喻：住客的"房号牌"】
//   WebSocket 是有状态长连接，每个连接需要绑定一个用户标识（UserId），用于在
//   WebSocketSessionManager 的登记簿里寻址、支持私聊/定向推送。本文件只定义这一个
//   类型别名，是整个 websocket/ 模块最底层的"字典条目"之一。
//
// 关键技术点（初学者重点理解）：
//   1. 【uint64_t 而非 int】用户 id 选 64 位无符号整数，容量足以支撑海量用户，
//      且与数据库自增主键/雪花 id 等常见方案对齐。用 int 会在 20 亿用户时溢出。
//   2. 【类型别名而非强类型】用 using UserId = uint64_t 而非 enum class，让 UserId
//      可直接当整数用，避免 handler 里频繁来回转换。代价是失去类型安全（UserId 与
//      普通 uint64_t 不区分），教学项目里这是可接受的折中。
//   3. 【0 表匿名】约定 uid==0 表示匿名用户，匿名用户不注册到 SessionManager——
//      WebSocketSessionManager::registerSession 对 uid==0 直接返回 false。
//   4. 【零依赖头文件】只 include <cstdint>，不依赖项目内任何其他头，可被任意
//      翻译单元包含，是最底层的类型地基。
// =============================================================================
#pragma once
#ifndef WEBSOCKET_USER_ID_H
#define WEBSOCKET_USER_ID_H

#include <cstdint>

// 用户标识类型：64 位无符号整数。0 约定为匿名用户（不注册到 SessionManager）。
// 用 using 别名而非强类型 enum class，方便 handler 直接当整数使用。
using UserId = uint64_t;

#endif