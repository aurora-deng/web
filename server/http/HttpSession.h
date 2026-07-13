#pragma once
#ifndef HTTP_SESSION_H
#define HTTP_SESSION_H
#include <coroutine>
#include"server/CoroutineScheduler/AWaiter.h"
#include"server/CoroutineScheduler/Task.h"
enum class AwaitType
{
    NONE,
    READ,
    WRITE,
    TIMER
};
class SubReactor;
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
    Connection* conn=nullptr;
public:
    CoroutineContext coroutine_context;

    explicit HttpSession(Connection* c,SubReactor* r):reactor(r),conn(c){

    }
    

    Task<void> run();
private:
    Task<HttpRequest> readRequest();
    Task<HttpResponse*> execute(HttpRequest& req);
    Task<void> send(HttpResponse* resp);
};



#endif 