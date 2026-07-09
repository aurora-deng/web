#include "AWaiter.h"


bool ReadAwaiter::await_ready()
{
    return false;
}

void ReadAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // reactor->scheduler.suspend(fd,EPOLLIN,h);
    auto& conn=reactor->conns[fd];          //映射找到东西
    conn.coroutine.handle=h;
    conn.coroutine.waitingRead=true;
}

void ReadAwaiter::await_resume()
{
}
