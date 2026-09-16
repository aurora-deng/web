// ==============================================================================
// 文件名：RequestContext.h
// 职责比喻：这是"请求档案袋"——一次请求的"全部手续文件夹"。
//   一次 HTTP 请求要经过解析→路由→执行→响应等多个环节，每个环节都需要访问请求和响应对象、
//   连接信息、路由参数、撤单信号等。RequestContext 把这些东西装进一个"档案袋"，在各环节之间传递，
//   避免到处传一长串参数。业务 handler 收到的就是这个档案袋，能取到所需的一切。
//
// 第四阶段为支持 WebSocket 做的改造：
//   - fd 改为默认初始化 -1（原为未初始化的 int），避免对象池复用时残留野值导致误用。
//   - 新增 webSocketAccepted 标志位（默认 false），业务 handler 调用 acceptWebSocket() 后置 true。
//   - 新增 acceptWebSocket() 方法，供业务侧表达"接受 WebSocket 升级"。
//   HttpSession 在发送响应前检查 webSocketAccepted，决定是否转入 WebSocketSession。
//
// 关键技术点（初学者重点理解）：
//   1. 【一次请求的载体】RequestContext 封装了 request（请求）、response（响应指针）、
//      fd（连接）、route（命中路由）、params（路径参数）、cancellation（撤单灯）等。
//      它不暴露 Session，Worker 不能越过 Reactor 直接操作连接状态。
//   2. 【response 是指针非对象】response 从对象池借出，用完归还，所以是指针。业务可直接
//      覆盖它（如换成错误响应），灵活度高。
//   3. 【WebSocket 升级标志】webSocketAccepted 让业务 handler 通过 acceptWebSocket() 表达
//      "我接受升级"，HttpSession 发响应前检查此标志决定是否转入 WebSocketSession。
// ==============================================================================
#pragma once
#ifndef EEQUEST_CONTEXT_H
#define EEQUEST_CONTEXT_H
#include <cstring>
#include "server/Executor/HandlerCancellation.h"
#include "server/http/http.h"
struct Connection;
struct RouteEntry;

/**
 * @brief 请求上下文——一次请求的"档案袋"
 *
 * 通俗解释：像快递员手里那个"配送单夹"，上面夹着：客户要什么（request）、回执单（response）、
 *   走哪个门（fd）、按哪条路线（route）、路线上的岔路口参数（params）和是否已撤单。
 *   整个配送过程就递这一个夹子，不用每次单独传一堆东西。
 */
struct RequestContext
{
    HttpRequest request;          // 本次请求对象（解析器填入）
    HttpResponse *response = nullptr; // 响应对象指针（对象池借出，用完归还）
    int fd = -1;                  // 连接 fd（默认 -1 避免野值，防止对象池复用时残留旧 fd 被误用）
    const RouteEntry *route = nullptr; // 命中的路由项
    HandlerCancellation cancellation; // Worker 的撤单信号与单调时钟截止时间

    std::unordered_map<std::string, std::string> params; // 路径参数（如 /user/:id 中的 id）

    bool handled = false;         // 是否已被某个中间件/handler 处理
    uint64_t Id = 0;              // 请求/连接标识，用于日志追踪

    // 业务 handler 调用 acceptWebSocket() 表示接受升级；
    // HttpSession 在发送响应前检查该标志并转入 WebSocketSession。
    // 默认 false：每轮请求默认不升级，业务需主动调 acceptWebSocket() 表达意愿——
    // 这样普通 HTTP 请求完全不受 WebSocket 逻辑影响。
    bool webSocketAccepted = false;
    bool sseAccepted = false;

    /** @brief 业务侧调用，表示接受 WebSocket 升级 */
    void acceptWebSocket()
    {
        webSocketAccepted = true;
        sseAccepted = false;
    }

    /** @brief 业务侧调用，表示发送 SSE 首部后把连接交给 SseSession */
    void acceptSse()
    {
        sseAccepted = true;
        webSocketAccepted = false;
    }

    /** 耗时 handler 应在循环、分批 I/O 或重计算边界调用。 */
    bool stopRequested() const noexcept
    {
        return cancellation.stopRequested();
    }

    bool handlerDeadlineExceeded() const noexcept
    {
        return cancellation.deadlineExceeded();
    }

    /**
     * @brief 取路径参数（路由匹配得到，如 /user/:id 的 id）
     * @param key 参数名
     * @return 参数值；不存在返回空串
     */
    std::string param(const std::string &key);

    /**
     * @brief 取查询参数（URL ? 后的键值对）
     * @param key 参数名
     * @return 参数值；不存在返回空串
     */
    std::string querry(const std::string &key);
};

#endif
