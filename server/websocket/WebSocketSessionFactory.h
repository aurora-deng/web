// =============================================================================
// 文件名：WebSocketSessionFactory.h
// 所属模块：server/websocket —— WebSocket 会话的专用装配台（2.0 新增）
//
// 【职责比喻：WebSocket 会话的"专用装配台"】
//   HTTP 升级 WebSocket 时，HttpSession 自己造不出 WebSocketSession——它得带上
//   manager / dispatcher / uid 等一坨 WebSocket 专属依赖，这些都在 websocket 模块里，
//   session 层不该直接 include。于是 SubReactor 持有一个 SessionFactory 抽象装配台，
//   升级时调 createWebSocketSession 要一个新会话。本类是那个抽象装配台的具体实现：
//   唯一知道"manager + dispatcher 怎么组装成 WebSocketSession"的地方，构造时把
//   manager/dispatcher 引用绑定好，createWebSocketSession 时把它们注入进新会话。
//
// 【2.0 依赖倒置的关键】
//   SubReactor 持有 sessionFactory_ 抽象指针（SessionFactory 基类），而非直接持有
//   wsManager_/wsDispatcher_ 具体类型。本具体工厂住在 websocket 模块里，构造时由
//   ReactorGroup 注入给每个 SubReactor。这样 session/transport 层不依赖 websocket 模块，
//   依赖方向单向干净——websocket 模块依赖 session 层，而非反过来。
//
// 【在系统中的角色】
//   本类是 2.0 依赖倒置的落地点。升级流程：
//     HttpSession 决定升级 → 调 SubReactor::sessionFactory()->createWebSocketSession(key, reactor, uid)
//     → 本工厂 make_shared<WebSocketSession>(key, reactor, uid, &manager_, dispatcher_)
//     → 返回 shared_ptr<Session>，HttpSession 把它 reset 进 Connection::session
//     → 旧 HTTP 协程 co_return，调度器启动新 WS 协程接管 fd。
//
// 关键技术点（初学者重点理解）：
//   1. 【依赖倒置】SubReactor 持有 SessionFactory* 抽象指针，不直接依赖 WebSocketSessionManager /
//       WebSocketDispatcher 具体类型。具体工厂实现住在 websocket 模块里，构造时注入。
//   2. 【唯一装配点】manager_ / dispatcher_ 的组装关系只在本文件出现——外部只调
//       createWebSocketSession(key, reactor, uid) 三个参数，不需要知道内部依赖。
//   3. 【override createWebSocketSession】实现基类纯虚，返回 shared_ptr<Session> 基类指针，
//       让 Connection::session 多态持有，与 HttpSession 共享同一基类接口。
//   4. 【引用成员】manager_ / dispatcher_ 是引用而非指针——它们由全局单例持有，生命周期
//       长于工厂本身，用引用表达"必须存在且不可空"的约束。
//   5. 【出站配合】工厂只负责"装配会话"，不掺和出站发送。装配出的 WebSocketSession 出站
//       同样走 OutboundTask + OutboundQueue + writerLoop 体系，与 HttpSession 一致。
// =============================================================================
#pragma once
#ifndef WEBSOCKET_SESSION_FACTORY_H
#define WEBSOCKET_SESSION_FACTORY_H

#include "server/session/SessionFactory.h"

// 前置声明：避免头文件循环依赖，.cpp 中才 #include 真正的定义
class WebSocketSessionManager;
class WebSocketDispatcher;

/**
 * @brief WebSocket 会话专用装配台——SessionFactory 的具体实现（2.0 新增）
 *
 * 【装配台 通俗解释】
 *   唯一知道"manager + dispatcher 怎么组装成 WebSocketSession"的地方。构造时把
 *   manager/dispatcher 引用绑定好；SubReactor 升级时调 createWebSocketSession，
 *   本工厂内部 make_shared<WebSocketSession> 把这些依赖注入进去，交回造好的会话。
 *
 * 【为何需要工厂而非直接 new 通俗解释】
 *   WebSocketSession 构造需要一坨 WebSocket 专属依赖（manager 管 uid 映射、dispatcher
 *   分发消息），这些依赖都在 websocket 模块里。若让 HttpSession 直接 new WebSocketSession，
 *   就得在 session 层 include websocket 头，依赖方向乱了。用工厂：websocket 模块提供工厂
 *   实现，构造时注入给 SubReactor，HttpSession 只调抽象接口，依赖方向单向干净。
 *
 * @note SubReactor 持有 SessionFactory* 抽象指针（基类），不直接依赖本具体类型。
 *       本类构造由 ReactorGroup 在初始化时完成，注入给每个 SubReactor。
 */
class WebSocketSessionFactory : public SessionFactory
{
public:
    /**
     * @brief 构造装配台，绑定 manager / dispatcher 引用
     * @param manager 全局 WebSocketSessionManager 引用（用于会话注册/注销/跨 Reactor 寻址）
     * @param dispatcher 全局 WebSocketDispatcher 引用（用于业务消息路由到 handler）
     * @note manager / dispatcher 由全局单例持有，生命周期长于工厂，用引用表达"不可空"。
     */
    WebSocketSessionFactory(WebSocketSessionManager &manager,
                            WebSocketDispatcher &dispatcher);

    /**
     * @brief 创建一个 WebSocketSession（override SessionFactory::createWebSocketSession）
     * @param key HTTP 升级后沿用的连接身份（fd + connId）
     * @param reactor 所属 SubReactor（事件循环 + 调度器 + OutboundQueue）
     * @param uid WebSocket 用户标识（0 表示匿名，匿名不注册到 manager）
     * @return 装配好的 WebSocketSession，以 shared_ptr<Session> 基类指针返回供多态持有
     *
     * 通俗解释：SubReactor 升级时调这个——给 key / reactor / uid，工厂内部把 manager_ /
     *   dispatcher_ 等依赖接好，make_shared<WebSocketSession> 造一个新会话交回来。
     *   返回 shared_ptr<Session> 是为了能直接 reset 进 Connection::session，与
     *   HttpSession 共享同一基类接口，升级时无缝替换。
     */
    std::shared_ptr<Session> createWebSocketSession(
        ConnectionKey key, SubReactor *reactor, UserId uid) override;

private:
    WebSocketSessionManager &manager_;   // 全局会话目录引用（不持有所有权，生命周期长于工厂）
    WebSocketDispatcher &dispatcher_;    // 全局业务派发器引用（不持有所有权，生命周期长于工厂）
};

#endif
