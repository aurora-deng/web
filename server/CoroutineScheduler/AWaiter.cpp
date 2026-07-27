#include "AWaiter.h"
#include "server/SubReactor/SubReactor.h"

bool ReadAwaiter::await_ready()
{
     // 连接已关闭时不能再登记 I/O 等待，直接让协程继续，由 Session 的下一次查表决定 co_return。
 // 这里使用 find 而非 operator[]，避免为已关闭 fd 意外创建“幽灵连接”。
    auto it = reactor->conns.find(fd);
    return it == reactor->conns.end() || it->second->state.closed;
}

bool ReadAwaiter:: await_suspend(std::coroutine_handle<> h)
{
    
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end() || it->second->state.closed)
        return false; // 连接已关闭，不挂起

    auto &conn = *it->second;
    conn.session->coroutine_context.handle = h;
    conn.session->coroutine_context.state=AwaitType::READ;
    conn.session->coroutine_context.waiting=true;
   
    // updateEvent 会根据背压决定是否保留 EPOLLIN，并以 ONESHOT 重新武装；
// I/O 到达后 Reactor 只负责把该句柄放回调度器队列。
    reactor->updateEvent(fd);
    return true;
}

void ReadAwaiter::await_resume()
{
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end() || it->second->state.closed)
        return; // 连接已关闭，不挂起
    auto &conn = *it->second;
    conn.session->coroutine_context.waiting=false;
}

bool WriteAwaiter::await_ready()
{
    // 修复5：挂起前检查连接是否已关闭
    auto it = reactor->conns.find(fd);
    return it == reactor->conns.end() || it->second->state.closed;
}

bool WriteAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 修复2+5：用 find 查找，不用 conns[fd]
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end() || it->second->state.closed)
        return false; // 连接已关闭，不挂起

    auto &conn = *it->second;
    conn.session->coroutine_context.handle = h;
    conn.session->coroutine_context.state = AwaitType::WRITE;
    conn.session->coroutine_context.waiting=true;

     // wantWrite 由 updateEvent 与读关注合并，避免注册 EPOLLOUT 时覆盖仍需要的 EPOLLIN。
    conn.state.wantWrite = true;

    reactor->updateEvent(fd);
    return true;
}

void WriteAwaiter::await_resume()
{
}