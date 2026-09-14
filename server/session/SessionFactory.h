// =============================================================================
// 文件名：SessionFactory.h
// 所属模块：server/session —— 协议会话的"装配台"接口（2.0 新增）
//
// 【职责比喻：协议会话的"装配台"】
//   HTTP 升级 WebSocket 时，HttpSession 自己造不出 WebSocketSession（它得带上 manager /
//   dispatcher / uid 等一堆 WebSocket 协议依赖，这些都在 websocket 模块里，HttpSession 不该
//   直接 include）。于是 SubReactor 持有一个 SessionFactory 装配台，HttpSession 升级时向它
//   要一个新会话，工厂内部把 manager/dispatcher 等依赖组装好再交出 WebSocketSession。
//   SubReactor 只持有本抽象接口，不直接依赖 WebSocket 具体类型——是 2.0 依赖倒置的关键。
//
// 关键技术点（初学者重点理解）：
//   1. 【依赖倒置】SubReactor 持有 SessionFactory* 抽象指针而非 WebSocketSessionManager /
//       WebSocketDispatcher 具体类型，切断 transport/session 层对 websocket 模块的直接依赖。
//       具体工厂实现住在 websocket 模块里，构造时注入给 SubReactor。
//   2. 【抽象接口】纯虚 createWebSocketSession = 0，不同实现可造不同配置的 WS 会话；
//       虚析构保证通过基类指针销毁子类工厂时正确析构。
//   3. 【升级交接点】HTTP→WebSocket 升级时，HttpSession 调本工厂 createWebSocketSession 拿到
//       新会话，reset 进 Connection::session，旧 HTTP 协程 co_return，新 WS 协程接管 fd。
//   4. 【返回 shared_ptr<Session>】工厂返回基类指针，让 Connection::session 多态持有，与
//       HttpSession 共享同一基类接口，升级时无缝替换。
//   5. 【2.0 出站配合】升级后的 WebSocketSession 出站同样走 OutboundTask + TransportWriter +
//       writerLoop 体系，与 HttpSession 一致；工厂只负责"装配会话"，不掺和出站发送。
// =============================================================================
#pragma once
#ifndef SESSION_FACTORY_H
#define SESSION_FACTORY_H

#include "server/websocket/WebSocketTypes/UserId.h"

#include <memory>
#include "server/transport/ConnectionKey.h"

class Session;
class SubReactor;

/**
 * @brief 协议会话装配台抽象接口
 *
 * 【装配台 通俗解释】
 *   一个"造会话"的接口：你说要一个 WebSocket 会话（给 fd / reactor / uid），它就内部组装好
 *   manager / dispatcher 等依赖，交回一个造好的 WebSocketSession。SubReactor 只认这个接口，
 *   不认 WebSocket 具体类型，从而把 session 层与 websocket 模块解耦。
 *
 * 【为何用抽象工厂而非直接 new 通俗解释】
 *   WebSocketSession 构造需要一坨 WebSocket 专属依赖（manager 管 uid 映射、dispatcher 分发
 *   消息），这些依赖都在 websocket 模块里。若让 HttpSession 直接 new WebSocketSession，
 *   就得在 session 层 include websocket 头，依赖方向乱了。用工厂：websocket 模块提供工厂
 *   实现，构造时注入给 SubReactor，HttpSession 只调抽象接口，依赖方向单向干净。
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
     * @param fd 连接的 socket fd
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
};

#endif
