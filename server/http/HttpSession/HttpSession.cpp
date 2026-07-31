#include "HttpSession.h"
#include "server/SubReactor/SubReactor.h"

/**
 * @brief 通过fd安全查找当前连接对象
 * @return 存在返回Connection裸指针，连接已销毁返回nullptr
 * @note 核心协程安全逻辑：
 * 协程挂起(co_await)期间，其他线程/协程可能执行fd_close销毁Connection；
 * 不能跨co_await缓存Connection指针，每次恢复执行必须重新查表，防止野指针段错误
 */
Connection *HttpSession::getConn()
{
    // fd仅作为哈希表查询key，无法保证当前Connection对象存活
    auto it = reactor->conns.find(fd);
    // 找不到代表连接已被关闭并从map移除
    if (it == reactor->conns.end())
        return nullptr;

    // 返回智能指针管理的裸指针，仅当前作用域临时使用
    return it->second.get();
}

/**
 * @brief 单次HTTP响应发送完成后的后置处理函数
 * @note 解决两大问题：1. 野指针段错误 2. 内存背压限流恢复读事件
 */
void HttpSession::afterSend()
{
    // 协程等待写事件后，连接可能已被销毁，必须重新查表获取连接
    auto it = reactor->conns.find(fd);
    // 连接不存在直接退出，不执行后续逻辑
    if (it == reactor->conns.end())
        return;
    // 本次循环全新的连接引用，规避旧指针悬空问题
    auto &conn_after_loop = *it->second;

    // ========== 内存背压限流恢复逻辑 ==========
    // 内存限流标记开启时，若读缓冲区剩余可读数据低于阈值，解除限流，恢复epoll读监听
    // MAX_PENDING_BYTES：全局最大待处理缓存阈值；阈值一半作为恢复水位，防止频繁开关读事件
    if (conn_after_loop.state.pauseByMemory &&
        conn_after_loop.readBuffer.readableBytes() < MAX_PENDING_BYTES / 2)
    {
        // 关闭内存限流标记，允许继续读取客户端请求
        conn_after_loop.state.pauseByMemory = false;
    }

    // 清空写事件标记，当前响应发送完毕，无剩余数据待发送
    conn_after_loop.state.wantWrite = false;
    if (keepAlive_)
    {
        // 长连接：更新epoll监听事件，继续监听下一次客户端读事件
        reactor->updateEvent(fd);
    }
    else
    {
        // 短连接：响应发送完成直接关闭fd，断开TCP连接
        reactor->fd_close(fd, "keepalive false", true);
    }
}

// 【协程发送响应函数，当前注释封存】
// bool HttpSession::sendResponse(HttpResponse &resp)
// {
//     while (true)
//     {
//         // 每次循环先安全获取连接，防止协程唤醒后连接已销毁
//         auto conn = getConn();
//         if (!conn || conn->state.closed)
//         {
//             // 连接失效，归还响应对象到内存池，避免内存泄漏
//             responsePool.release(&resp);
//             co_return false;
//         }
//         // 调用底层reactor发送响应体数据
//         auto state = reactor->sendBody(fd, resp);
//         switch (state)
//         {
//         case SEND_OK:
//             // 数据全部发送完成，归还响应对象，执行发送后置逻辑
//             responsePool.release(&resp);
//             afterSend();
//             co_return true;

//         case SEND_AGAIN:
//             // 非阻塞socket缓冲区满，发送被截断；挂起协程等待EPOLLOUT可写事件
//             co_await WriteAwaiter(reactor, fd);
//             // 等待唤醒后重新进入循环，继续发送剩余数据
//             break;

//         default:
//             // 发送出现IO错误，释放响应资源，关闭TCP连接
//             responsePool.release(&resp);
//             reactor->fd_close(fd, "send error", true);
//             co_return false;
//         }
//     }
// }

// 【HTTP请求执行业务逻辑，当前注释封存】
// bool HttpSession::execute(RequestContext &ctx)
// {
//     // 安全获取连接，连接销毁直接返回失败
//     auto conn = getConn();
//     if (!conn)
//         return false;

//     // 从内存池创建空响应对象，复用内存减少分配开销
//     ctx.response = std::make_shared<HttpResponse>();
//     // 路由处理器执行业务逻辑
//     bool ok = reactor->router.handle(ctx);
//     // 路由处理失败且无响应，默认返回500服务端错误
//     if (!ok && !ctx.response)
//     {
//         ctx.response->status = 500;
//     }
//     // 将当前响应存入连接响应队列
//     conn->responses.push_back(*ctx.response);

//     return true;
// }

/**
 * @brief 读取并解析客户端HTTP请求
 * @return RequestReadResult 读取解析结果枚举
 * CLOSED：对端关闭连接 / 连接已销毁
 * ERROR：HTTP报文格式错误
 * COMPLETE：完整HTTP请求解析成功
 * TOO_LARGE：缓冲区堆积数据超限，触发内存限流
 * NEED_MORE：缓冲区数据不足，需要继续recv读取数据
 * @note 分层设计：优先解析现有缓冲区，减少系统recv调用；粘包自动处理
 */
RequestReadResult HttpSession::readRequest()
{
    // 第一步安全校验连接状态
    auto conn = getConn();
    if (!conn || conn->state.closed)
        return RequestReadResult::CLOSED;
    // 更新会话状态：当前正在解析HTTP报文
    state = SessionState::PARSING;
    // 调用HTTP编解码器解析读缓冲区数据
    auto parseState = reactor->codec.decode(
        conn->readBuffer, parser_, context_.request, keepAlive_);

    // 分支1：完整请求解析成功
    if (parseState == PARSE_OK)
    {
        // 若客户端提前关闭写端，强制关闭长连接
        if (conn->state.peerClosed)
            keepAlive_ = false;
        // 更新当前缓冲区待处理字节数
        conn->pendingBytes = conn->readBuffer.readableBytes();
        // 缓冲区数据低于阈值，解除内存限流
        if (conn->pendingBytes < MAX_PENDING_BYTES)
            conn->state.pauseByMemory = false;
        return RequestReadResult::COMPLETE;
    }

    // 分支2：报文解析出错（非法HTTP协议）
    if (parseState == PARSE_ERROR)
        return RequestReadResult::ERROR;

    // 分支3：缓冲区数据不足，需要调用recv读取新数据
    state = SessionState::READING;
    // 底层reactor执行非阻塞recv，填充readBuffer
    const auto recvState = reactor->recvSocket(fd);
    // recv检测到TCP连接关闭
    if (recvState == RecvState::CLOSED)
        return RequestReadResult::CLOSED;

    // recv完成后协程恢复，必须重新校验连接是否存活
    conn = getConn();
    if (!conn || conn->state.closed)
        return RequestReadResult::CLOSED;

    // 再次解析新增数据后的缓冲区
    state = SessionState::PARSING;
    parseState = reactor->codec.decode(
        conn->readBuffer, parser_, context_.request, keepAlive_);
    // 二次解析成功
    if (parseState == PARSE_OK)
    {
        if (conn->state.peerClosed)
            keepAlive_ = false;
        conn->pendingBytes = conn->readBuffer.readableBytes();
        if (conn->pendingBytes < MAX_PENDING_BYTES)
            conn->state.pauseByMemory = false;
        return RequestReadResult::COMPLETE;
    }
    
    // 二次解析报文错误
    if (parseState == PARSE_ERROR)
        return RequestReadResult::ERROR;

    // recv触发内存限流：缓冲区数据超限，暂停读事件
    if (recvState == RecvState::PAUSED)
        return RequestReadResult::TOO_LARGE;

    // 客户端关闭TCP写端，但缓冲区无完整请求，判定为非法断连，关闭fd
    if (conn->state.peerClosed)
    {
        reactor->fd_close(fd, "peer closed with incomplete request", true);
        return RequestReadResult::CLOSED;
    }

    // 数据依旧不足，等待下一次EPOLLIN可读事件
    return RequestReadResult::NEED_MORE;
}

/**
 * @brief HTTP会话主协程循环
 * @details 单个TCP连接的生命周期全部在此协程内循环处理：
 * 1. 重置请求上下文，隔离keep-alive多请求状态
 * 2. 循环读取、解析客户端HTTP请求
 * 3. 提交业务处理任务至线程池，协程挂起等待处理完成
 * 4. 构建HTTP响应，循环非阻塞发送全部响应数据
 * 5. 根据keep-alive判断是否复用连接，循环处理下一个请求
 * @note 全程不跨co_await缓存Connection*，每次唤醒后重新查表，彻底规避野指针；
 * 内存池复用Request/Response对象，减少频繁内存分配释放；
 * 业务逻辑异步至线程池，不阻塞Reactor事件循环
 */
Task<void> HttpSession::run()
{
    // 长连接循环：单个TCP连接持续处理多个HTTP请求，直到短连接/异常关闭
    while (true)
    {
        // ========== 请求上下文重置 ==========
        // keep-alive下多个请求共用同一个context，必须完全清空上一轮请求状态
        // 原位清空复用容器内存容量，避免频繁malloc/free造成性能损耗
        context_.request.reset();         // 清空上一轮HTTP请求报文
        context_.response = nullptr;      // 释放上一轮响应智能指针
        context_.route = nullptr;         // 清空路由匹配记录
        context_.params.clear();          // 清空URL路由参数
        context_.handled = false;         // 重置业务处理标记
        context_.Id = 0;                   // 请求唯一ID清零
        context_.session = this;           // 绑定当前会话指针
        context_.fd = fd;                  // 绑定当前连接fd
        keepAlive_ = true;                 // 默认开启长连接，由报文/业务覆盖

        // 上下文重置完成，校验当前连接是否已销毁
        auto conn = getConn();
        if (!conn || conn->state.closed)
        {
            state = SessionState::CLOSED;
            co_return; // 连接销毁，退出会话协程
        }
        state = SessionState::READING;

        // 内层循环：持续读取数据直到解析出完整HTTP请求
        while (true)
        {
            const auto result = readRequest();
            if (result == RequestReadResult::COMPLETE)
                break; // 完整请求解析成功，退出读循环执行业务

            // TCP连接关闭，直接退出协程
            if (result == RequestReadResult::CLOSED)
            {
                state = SessionState::CLOSED;
                co_return;
            }
            // 报文错误 / 缓冲区超限，关闭连接退出协程
            if (result == RequestReadResult::ERROR ||
                result == RequestReadResult::TOO_LARGE)
            {
                state = SessionState::CLOSED;
                // 根据错误类型打印日志，关闭fd
                reactor->fd_close(fd,
                                  result == RequestReadResult::ERROR?
                                   "malformed HTTP request": "request exceeds read buffer limit",
                                  true);
                co_return;
            }
            // 数据不足，挂起协程等待EPOLLIN客户端可读事件
            co_await ReadAwaiter(reactor, fd);
        }

        // ========== 执行业务路由处理 ==========
        state = SessionState::EXECUTING;
        // 协程唤醒后重新校验连接状态
        conn = getConn();
        if (!conn || conn->state.closed)
        {
            state = SessionState::CLOSED;
            co_return;
        }

        // 保存连接唯一ID，用于线程池任务完成唤醒匹配
        uint64_t connId = conn->id;
        // 提交业务处理任务至线程池executor
        const bool submitted = reactor->executor.submit([this, connId]()
                                                        {
            try
            {
                // 编解码器分发路由，执行业务handler
                bool dispatched = this->reactor->codec.dispatch(context_);
                // 路由分发失败且无响应，归还空响应至内存池
                if (!dispatched && context_.response)
                {
                    responsePool.release(context_.response);
                    context_.response = nullptr;
                }
            }
            catch (...)
            {
                // 捕获业务handler所有异常，防止线程池崩溃（std::terminate）
                // 异常统一封装500错误响应返回客户端
                if (!context_.response)
                    context_.response = responsePool.acquire();
                context_.response->reset();
                context_.response->status = 500;
                context_.response->statusText = "Internal Server Error";
                context_.response->text("Internal Server Error");
            }
            // 无论业务处理成功/异常，必须通知Reactor任务执行完成
            // 两种场景：
            // 1. 连接存活：唤醒当前HttpSession协程，执行发送响应逻辑
            // 2. 连接已销毁：通过僵尸连接队列标记，协程唤醒后安全退出无段错误
            reactor->notifyExecuteComplete(this->fd, connId);
        });
        // 若遇到/slow
        if (submitted)
        {
            // 任务提交成功，挂起协程等待线程池完成通知,交出协程序权柄，等待线程池完成通知,回复/fast
            co_await ExecuteAwaiter(reactor, fd);
        }
        else
        {
            // 线程池任务队列已满，触发过载背压，直接返回503服务不可用
            context_.response = responsePool.acquire();
            context_.response->status = 503;
            context_.response->statusText = "Service Unavailable";
            context_.response->keepAlive = false; // 过载不保持长连接
            context_.response->text("Service Unavailable");
        }

        // ========== 业务处理完成，准备发送响应 ==========
        // 协程唤醒后再次校验连接
        conn = getConn();
        if (!conn || conn->state.closed)
        {
            // 连接销毁，归还响应内存池资源防止泄漏
            if (context_.response)
                responsePool.release(context_.response);
            context_.response = nullptr;
            state = SessionState::CLOSED;
            co_return;
        }
        auto *response = context_.response;
        // 无有效响应对象，关闭连接
        if (!response)
        {
            reactor->fd_close(fd, "request dispatch failed", true);
            state = SessionState::CLOSED;
            co_return;
        }

        // ========== 长连接协商逻辑 ==========
        // 三个条件同时满足才保持长连接：
        // 1. 客户端未提前关闭TCP写端
        // 2. 业务handler允许长连接
        // 3. 当前会话标记keepAlive_开启
        if (conn->state.peerClosed)
            keepAlive_ = false;
        response->keepAlive = response->keepAlive && keepAlive_;
        keepAlive_ = response->keepAlive;
        // 序列化HTTP响应头部，填充Content-Length等字段
        response->buildHeader();
        state = SessionState::WRITING;

        // 内层循环：循环发送响应全部数据，处理非阻塞缓冲区满场景
        while (true)
        {
            // 每次发送前安全校验连接
            conn = getConn();
            if (!conn || conn->state.closed)
            {
                responsePool.release(response);
                context_.response=nullptr;
                state = SessionState::CLOSED;
                co_return;
            }
            // 调用底层发送器发送响应数据,触发发送函数
            const auto sendState = reactor->sender.send(fd, *response);
            switch (sendState)
            {
            case SEND_OK:
                // 响应完整发送完毕，释放响应内存，执行发送后置逻辑
                responsePool.release(response);
                context_.response = nullptr;
                afterSend();
                break;

            case SEND_AGAIN:
                // socket发送缓冲区已满，非阻塞send返回EAGAIN；
                // 保留响应发送偏移量，挂起协程等待EPOLLOUT可写事件
                co_await WriteAwaiter(reactor, fd);
                continue; // 唤醒后重新进入循环续发剩余数据

            default:
                // 发送IO异常，释放资源，关闭TCP连接
                responsePool.release(response);
                context_.response=nullptr;
                reactor->fd_close(fd, "send error", true);
                state = SessionState::CLOSED;
                co_return;
            }
            break;
        }
        // 单次请求响应发送完毕，回到外层循环，重置上下文处理下一个请求
    }
}