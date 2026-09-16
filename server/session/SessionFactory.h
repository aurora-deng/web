// =============================================================================
// 文件名：SessionFactory.h
// 所属模块：server/session —— 协议会话的"装配台"接口（2.0 新增）
//
// 【职责比喻：协议会话的"装配台"】
//   HttpSession 自己造不出 WebSocketSession 或 SseSession：它们各自需要 manager、dispatcher、
//   uid/clientId 等协议依赖，而 HTTP 层不该直接 include 这些具体类型。于是 SubReactor 持有一个
//   SessionFactory 装配台，HttpSession 交接长连接时向它要新会话，具体工厂负责组装依赖。
//   SubReactor 只持有本抽象接口，不依赖 WS/SSE 具体类型——是 2.0 依赖倒置的关键。
//
// 关键技术点（初学者重点理解）：
//   1. 【依赖倒置】SubReactor 持有 SessionFactory* 抽象指针而非 WebSocketSessionManager /
//       SseSessionManager 等具体类型，切断事件循环层对上层协议模块的直接依赖。
//       ProtocolSessionFactory 在 Runtime 装配时注入给 SubReactor。
//   2. 【抽象接口】两个纯虚 create 方法分别创建 WS/SSE 会话；
//       虚析构保证通过基类指针销毁子类工厂时正确析构。
//   3. 【交接点】HTTP 101 或 SSE 200 首部发完后，HttpSession 调工厂拿到新会话，放进
//       Connection::session；旧 HTTP 协程退出，新长会话协程接管同一个 fd。
//   4. 【返回 shared_ptr<Session>】工厂返回基类指针，让 Connection::session 多态持有，与
//       HttpSession 共享同一基类接口，升级时无缝替换。
//   5. 【2.0 出站配合】WS 帧和 SSE chunk 都走 OutboundTask + TransportWriter + writerLoop；
//       工厂只负责"装配会话"，不掺和出站发送。
// =============================================================================
#pragma once
#ifndef SESSION_FACTORY_H
#define SESSION_FACTORY_H

#include "server/websocket/WebSocketTypes/UserId.h"

#include <cstdint>
#include <memory>
#include "server/transport/ConnectionKey.h"

class Session;
class SubReactor;

/**
 * @brief 协议会话装配台抽象接口
 *
 * 【装配台 通俗解释】
 *   一个"造会话"的接口：你提供连接身份、Reactor 和业务身份，它就组装好所需依赖，
 *   交回 WebSocketSession 或 SseSession。SubReactor 只认这个接口，不认具体协议类型。
 *
 * 【为何用抽象工厂而非直接 new 通俗解释】
 *   长会话构造需要协议专属依赖。若让 HttpSession 直接 new 具体 Session，HTTP 层就会反向
 *   依赖 websocket/sse 模块。ProtocolSessionFactory 集中在 Runtime 装配这些依赖，
 *   HttpSession 只调用抽象接口，依赖方向保持单向。
 *
 * @note 2.0 新增：SubReactor 持有 sessionFactory_ 而非直接依赖 wsManager_/wsDispatcher_。
 *       配合 OutboundQueue，SubReactor 对外只暴露两个抽象：会话工厂 + 出站队列，协议相关
 *       细节全部下沉到子类 / 工厂实现。
 */
class SessionFactory
{
public:
    virtual ~SessionFactory() = default;  // 多态析构：基类指针销毁工厂实现时必须虚析构

    /**
     * @brief 创建一个 WebSocketSession
     * @param key 连接身份（fd + connId）
     * @param reactor 所属 SubReactor（事件循环 + 调度器 + 出站队列）
     * @param uid WebSocket 用户标识（用于 manager 注册与广播寻址）
     * @return 装配好的 WebSocketSession，以 Session 基类指针返回供多态持有
     *
     * 【通俗解释】升级时 HttpSession 调这个：给 fd / reactor / uid，工厂内部把 manager /
     *   dispatcher 等依赖接好，造一个造好的 WebSocketSession 交回来。返回 shared_ptr<Session>
     *   是为了能直接 reset 进 Connection::session，与 HttpSession 共享同一基类接口。
     */
    virtual std::shared_ptr<Session> createWebSocketSession(
        ConnectionKey key, SubReactor *reactor, UserId uid) = 0;

    /** 创建一个在 HTTP 200 首部发完后接管连接的 SSE 长会话。 */
    virtual std::shared_ptr<Session> createSseSession(
        ConnectionKey key,
        SubReactor *reactor,
        std::uint64_t clientId) = 0;
};

#endif
