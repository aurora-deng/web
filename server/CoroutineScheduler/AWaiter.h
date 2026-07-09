#pragma once
#ifndef AWAITER_H
#define AWAITER_H
#include <coroutine>
#include"server/SubReactor/SubReactor.h"
class SubReactor;



class ReadAwaiter
{
public:
    ReadAwaiter(SubReactor *reactor, int fd):reactor(reactor),fd(fd){}
    bool await_ready();
    void await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    int fd;
    SubReactor *reactor;
};

#endif