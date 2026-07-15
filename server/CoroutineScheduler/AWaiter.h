#pragma once
#ifndef AWAITER_H
#define AWAITER_H
#include <coroutine>
// #include"server/SubReactor/SubReactor.h"
// 前向声明，避免与 SubReactor.h 的循环依赖,然后再实现的cpp文件使用具体的函数引用，防止循环依赖
class SubReactor;
struct Connection;

class ReadAwaiter
{
public:
    ReadAwaiter(SubReactor *reactor, int fd) : reactor(reactor), fd(fd) {}
    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    void await_resume();
    // auto operator co_await()
    // {
    //     return ReadAwaiter{handle};
    // }
private:
    int fd;
    SubReactor *reactor;
    Connection *conn;
};

class WriteAwaiter
{
public:
    WriteAwaiter(SubReactor *reactor, int fd) : reactor(reactor), fd(fd) {}
    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    int fd;
    SubReactor *reactor;
    Connection *conn;

};

#endif