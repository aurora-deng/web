// =============================================================================
// 文件名：ServerRuntime.h
// ------------------------------------------------------------
// 【职责比喻：工厂总装线 / 酒店总经理】
// 声明服务器运行时 ServerRuntime——整个服务器的"总指挥"（工厂总装线/酒店总经理），
// 统一管理生命周期：监听套接字、MainReactor 的 epoll、ReactorGroup（一组 SubReactor）、
// Router、HTTP/WS 两个 Executor。所有组件在这里装配、启动、退出。
//
// 【生活比喻】
// ServerRuntime 是酒店"总经理"：手底下管着前台接待（acceptLoop）、一群楼层经理
// （ReactorGroup/SubReactor）、HTTP 与 WS 两间独立后厨（Executor）、一本菜谱（Router）。
// 开店时总经理先布置好前台和楼层经理，自己坐在前台等客人来（acceptLoop 阻塞）；
// 客人来了一位，总经理分给下一个楼层经理接待；接到 Ctrl+C 信号，总经理先放掉前台
// （releaseListener 释放端口），再依次让各楼层经理收工（stop+join）。
//
// 【第四阶段重构动机：不再创建 codec 实例 + SessionFactory 注入】
// 第四阶段前 ServerRuntime 持有 HttpCodec codec_ 成员，并把它下传给 ReactorGroup →
// SubReactor → 所有连接共享。那时只有 HTTP 一种协议，全局共享一个 codec 没问题。
// 引入 WebSocket 后，HTTP 与 WS 编解码互不兼容，共享 codec 会串扰。故重构为：
//   - 删除 HttpCodec codec_ 成员——Runtime 不再创建 codec 实例；
//   - 构造 ReactorGroup 时改传 *router_（路由器协议无关，HTTP/WS 共用）；
//   - codec 下沉到各 Session 子类内部（HttpSession 自带 HttpCodec，
//     WebSocketSession 自带 WebSocketCodec），每条连接独立编解码互不干扰；出站发送统一走 OutboundTask，不在 Session 抽象 sender。
//   - Runtime 创建 WebSocketSessionFactory（即 SessionFactory 的具体实现）wsFactory_，
//     经 ReactorGroup::start → SubReactor::setSessionFactory 注入到每个 SubReactor——
//     SubReactor 收到 HTTP Upgrade: websocket 时通过它创建 WebSocketSession，本类与
//     SubReactor 都不直接 new WebSocketSession，依赖倒置到 SessionFactory 抽象。
// 这是为支持多协议而做的依赖倒置：协议相关细节下沉到 Session，Runtime 只装配协议无关组件。
//
// 关键技术点（初学者重点理解）：
// 1. 停机顺序很关键：先 stop+join Reactor 线程并点亮在途 handler 的撤单灯；但暂时
//    保留 Reactor 对象和 eventfd。随后排空 HTTP/WS Executor，最后才 reset ReactorGroup。
//    这样 Worker 的完成通知始终有一个仍存活的回调目标。
// 2. 信号处理用 static g_runtime + handleStopSignal：信号处理函数只能做 async-signal-safe
//    操作，所以只置位 + 写 eventfd 唤醒 acceptLoop，由主循环走优雅退出流程。
// 3. wakeFd_ + acceptLoop 的 -1 超时：主 acceptor 用 epoll_wait 永久阻塞等新连接，
//    需要一个 wakeFd_ 让 stop() 能唤醒它退出。
// 4. maxConnections_ 全局限流：accept 时检查 reactorGroup_->activeConnections()，
//    超额直接 close 新 fd，避免无界占满 fd/内存。
// 5. 组件装配点：本类是整个服务器的"总装线"——构造期建 router_/httpExecutor_/
//    wsExecutor_/wsManager_/
//    wsDispatcher_/wsDelivery_/wsFactory_/reactorGroup_，start() 时再 setupListener + createReactors
//    把监听端和反应器组拉起，最后 acceptLoop 阻塞接客。其中 wsFactory_（SessionFactory
//    实现）在构造 ReactorGroup 时引用传入，由 ReactorGroup::start 注入每个 SubReactor。
// 6. wsDelivery_ 是可靠消息的生命周期边界：Reactor 启动后启动，退出时先 stop+join，
//    防止后台重试线程向正在销毁的 Reactor 投递任务。
// =============================================================================
#pragma once
#ifndef SERVER_RUNTIME_H
#define SERVER_RUNTIME_H

#include <memory>
#include <cstddef>
#include <atomic>
#include "server/Route/Router.h"
#include "server/Executor/Executor.h"
#include "server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketDelivery/WebSocketDeliveryService.h"
#include "server/websocket/WebSocketSessionFactory.h"

class ReactorGroup;
/**
 * @brief 服务器运行时（工厂总装线），封装所有基础设施生命周期
 *
 * 管理：监听套接字、epoll、SubReactor 组、Router 和 HTTP/WS 独立 Executor。
 * 协议 Codec 由各 Session 自持，不再在 Runtime 层持有 HttpCodec；出站发送由 OutboundTask + TransportWriter 体系承担。
 *
 * 【通俗解释】
 * 本类是整个服务器的"总装线"——所有组件在这里装配、启动、退出：
 * 构造期建 router_/httpExecutor_/wsExecutor_/wsManager_/wsDispatcher_/wsDelivery_/wsFactory_/reactorGroup_，
 * start() 时 setupListener + createReactors 拉起监听端和反应器组，最后 acceptLoop 阻塞接客。
 * 其中 wsFactory_（WebSocketSessionFactory，即 SessionFactory 实现）经 ReactorGroup
 * 注入到每个 SubReactor，让协议升级在 SubReactor 内部就能完成。
 *
 * 使用方式：
 * @code
 * ServerRuntime server;
 * server.router().GET("/", handler);
 * server.start();  // 阻塞在 acceptor loop；SIGINT/SIGTERM 触发优雅退出
 * @endcode
 *
 * @note 第四阶段重构后，本类不再持有 HttpCodec codec_ 成员——codec 已下沉到各 Session
 *       子类内部（HttpSession/WebSocketSession 各自持有），Runtime 只装配协议无关组件。
 */
class ServerRuntime
{
public:
    ServerRuntime();
    ~ServerRuntime();

    /** @return 路由器引用，外部用 server.router().GET(...) 注册路由 */
    Router &router() { return *router_; }

    /** @return WebSocket 消息分发器，按 type 注册业务 handler */
    WebSocketDispatcher &wsDispatcher() { return wsDispatcher_; }

    /** @brief 设置监听端口，默认 8080 */
    void setPort(int port) { port_ = port; }

    /** @brief 设置 SubReactor 数量，0=按 CPU 核数自动（默认） */
    void setReactorCount(size_t n) { reactorCount_ = n; }

    /** @brief 设置最大并发连接数（全局限流），默认 10000 */
    void setMaxConnections(size_t n) { maxConnections_ = n; }

    /**
     * @brief 请求服务器停止（线程安全）
     *
     * 【通俗解释】
     * 信号处理函数调用：置 running_=false 并写 wakeFd_ 唤醒 acceptLoop。
     * 只做这两件 async-signal-safe 的事，剩下交给主循环优雅退出。
     */
    void requestStop();

    /**
     * @brief 立即释放监听套接字（不阻塞）
     *
     * 【通俗解释】
     * 把 8080 端口还给系统，不必等 Reactor 全部 join 完——这样重启服务时能立即绑定。
     */
    void releaseListener();

    /**
     * @brief 启动服务器：建监听→建 Reactor→装信号→跑 acceptLoop→优雅退出
     *
     * 【通俗解释】
     * 总经理开店全流程：①布置前台（setupListener）；②招楼层经理（createReactors）；
     * ③登记信号处理（Ctrl+C 触发 handleStopSignal）；④自己坐前台等客人（acceptLoop 阻塞）；
     * ⑤退出循环后释放前台端口（releaseListener）；⑥通知楼层经理收工（stop+join）。
     */
    void start();

    WebSocketSessionManager &wsManager() { return wsManager_; }
    /** @return 可靠 WS 投递服务；业务 handler 用它提交带 ID/ACK 的消息。 */
    WebSocketDeliveryService &wsDelivery() { return wsDelivery_; }

private:
    // 路由表（协议无关）：第四阶段前 Runtime 还持有一个 HttpCodec codec_ 成员，
    // 引入 WebSocket 后已删除——codec 下沉到各 Session 子类内部，Runtime 只持有 Router。
    std::shared_ptr<Router> router_;
    WebSocketDispatcher wsDispatcher_;
    WebSocketSessionManager wsManager_;
    WebSocketDeliveryService wsDelivery_;
    // 析构前由 shutdownComponents 显式排空两个 Executor，再 reset ReactorGroup；不能只依赖
    // 成员逆序析构，因为 Worker 完成时仍要调用 Reactor 的完成通知入口。
    Executor httpExecutor_;                          // HTTP 业务舱：不会被 WS 慢任务占满
    Executor wsExecutor_;                            // WebSocket 业务舱：独立容量与截止时间
    std::unique_ptr<WebSocketSessionFactory> wsFactory_;  // SessionFactory 具体实现：构造期创建，经 ReactorGroup::start 注入每个 SubReactor，用于 HTTP→WebSocket 协议升级时 new WebSocketSession
    std::unique_ptr<ReactorGroup> reactorGroup_;
    int port_ = 8080;                                // 监听端口
    size_t reactorCount_ = 0;                        // SubReactor 数量（0=自动）
    size_t maxConnections_ = 10000;                  // 全局最大连接数（限流）
    int listenFd_ = -1;                              // 监听套接字 fd
    int epfd_ = -1;                                  // 主 acceptor 的 epoll fd
    int wakeFd_ = -1;                                // 用于唤醒 acceptor 退出的 eventfd
    std::atomic<bool> running_{true};                // 运行标志，控制 acceptLoop 退出

    /** @brief 创建监听套接字并注册到 epoll */
    void setupListener();
    /** @brief 创建并启动 SubReactor 组 */
    void createReactors();
    /** @brief acceptor 事件循环：accept 新连接并 dispatch 到 SubReactor */
    void acceptLoop();
    /** @brief 按“停生产者→排空消费者→销毁回调目标”的顺序关闭运行图（幂等） */
    void shutdownComponents() noexcept;
};

#endif
