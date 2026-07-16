#pragma once
#ifndef HTTP_SESSION_H
#define HTTP_SESSION_H
#include <coroutine>
#include"server/CoroutineScheduler/AWaiter.h"
#include"server/CoroutineScheduler/Task.h"
#include"server/http/HttpSession.h"
#include"server/http/http.h"

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
    int fd=-1;
public:
    CoroutineContext coroutine_context;
    explicit HttpSession(int fd,SubReactor* r):reactor(r),fd(fd){

    }
    
    void afterSend();

    Task<void> run();
private:    
    Task<HttpRequest> readRequest();
    Task<HttpResponse*> execute(HttpRequest& req);
    Task<void> send(HttpResponse* resp);
    bool readSocket();

};



#endif 