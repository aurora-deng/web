#pragma once
#ifndef HTTP_SESSION_H
#define HTTP_SESSION_H
#include <coroutine>
#include"server/CoroutineScheduler/AWaiter.h"
#include"server/CoroutineScheduler/Task.h"
#include "server/CoroutineScheduler/CoroutineScheduler.h"
#include"server/http/RequestContext/RequestContext.h"

class SubReactor;
enum class SessionState
{
    READ_REQUEST,
    EXECUTE,
    SEND_RESPONSE,
    CLOSED
};
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
    SessionState state=SessionState::READ_REQUEST;
public:
    CoroutineContext coroutine_context;
    explicit HttpSession(int fd,SubReactor* r):reactor(r),fd(fd){

    }
    Connection * getConn();
    void afterSend();
    bool sendResponse(RequestContext &ctx);
    bool execute(RequestContext &ctx);
    bool readRequest(RequestContext &ctx);
    Task<void> run();   

};



#endif 