#pragma once
#ifndef SERVER_RUNTIME_H
#define SERVER_RUNTIME_H

#include <memory>
#include <cstddef>
#include "server/Route/Router.h"
#include "server/Executor/Executor.h"
#include "server/http/HttpCodec/HttpCodec.h"

class ReactorGroup;
/**
 * @brief 服务器运行时，封装所有基础设施生命周期
 *
 * 管理：监听套接字、epoll、SubReactor 组、Router、Codec 和共享 Executor
 *
 * 使用方式：
 * @code
 * ServerRuntime server;
 * server.router().GET("/", handler);
 * server.start();  // 阻塞在 acceptor loop
 * @endcode
 */
class ServerRuntime
{
public:
    // 初始化赋值，决定给予的reactor数量（由核数决定）
    ServerRuntime();
    ~ServerRuntime();

    /**
     * @brief 获取路由器引用，用于注册路由和中间件
     */
    Router &router() { return *router_; }

    /**
     * @brief 设置监听端口
     */
    void setPort(int port) { port_ = port; }

    /**
     * @brief 设置 SubReactor 数量，0=自动检测 CPU 核心数
     */
    void setReactorCount(size_t n) { reactorCount_ = n; }

    /**
     * @brief 启动服务器：创建监听套接字、SubReactor，进入 acceptor loop
     */
    void start();

private:
    std::shared_ptr<Router> router_;
    HttpCodec codec_;
    std::unique_ptr<ReactorGroup> reactorGroup_;
    // 声明在 ReactorGroup 之后，析构时会先 drain Worker；Reactor 对象仍保持存活。
    Executor executor_;
    int port_ = 8080;
    size_t reactorCount_ = 0;
    int listenFd_ = -1;
    int epfd_ = -1;
    // 设置监听
    void setupListener();
    void createReactors();
    void acceptLoop();
};

#endif
