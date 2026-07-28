#pragma once
#ifndef HTTP_SESSION_H
#define HTTP_SESSION_H
#include <coroutine>
#include"server/CoroutineScheduler/AWaiter.h"
#include"server/CoroutineScheduler/Task.h"
#include "server/CoroutineScheduler/CoroutineScheduler.h"
#include"server/http/RequestContext/RequestContext.h"
#include "server/http/HttpParser/HttpParser.h"

class SubReactor;
// SessionState 描述一个连接上串行请求的主流程。状态显式化后，读等待、业务执行和写等待
// 不会在回调中交叉重入，也为后续增加观测指标或超时策略提供稳定阶段。
enum class SessionState
{
    READING,
    PARSING,
    EXECUTING,
    WRITING,
    CLOSED
};
enum class RequestReadResult
{
    // NEED_MORE 才允许挂起等待 EPOLLIN；CLOSED/ERROR/TOO_LARGE 都必须结束当前协程，
    // 以免错误连接重新进入 epoll 或继续占用缓冲区。
    COMPLETE,
    NEED_MORE,
    CLOSED,
    ERROR,
    TOO_LARGE
};

// CoroutineContext 是 Session 与 Reactor 之间的“挂起登记”，不拥有句柄的销毁权；
// CoroutineScheduler 才是最终回收者。state 用于匹配读写事件，waiting 便于表达是否正等待 I/O。
struct CoroutineContext{
     std::coroutine_handle<> handle;
    // 标志协程的状态
    AwaitType state=AwaitType::NONE;
    bool waiting=false;
};
struct Connection;

// 用于管理通信的暂存体
class HttpSession
{
private:
    SubReactor* reactor=nullptr;
    int fd=-1;
     // HTTP 生命周期完全属于 Session；Connection 只保留 fd、Buffer 和套接字状态。
    HttpParser parser_;
    RequestContext context_;
    bool keepAlive_=true;
    SessionState state=SessionState::READING;
public:
    CoroutineContext coroutine_context;
    explicit HttpSession(int fd,SubReactor* r):reactor(r),fd(fd){

    }
    Connection * getConn();
    void afterSend();
    RequestReadResult readRequest();
    SessionState sessionState() const { return state; }
    Task<void> run();    

};



#endif 