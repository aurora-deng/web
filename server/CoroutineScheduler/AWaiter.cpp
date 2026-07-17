#include "AWaiter.h"
#include "server/SubReactor/SubReactor.h"

bool ReadAwaiter::await_ready()
{
    // 修复5：挂起前检查连接是否已关闭
    // 如果连接已关闭或不存在，不挂起，让 co_await 立即继续
    auto it = reactor->conns.find(fd);
    return it == reactor->conns.end() || it->second->state.closed;
}

bool ReadAwaiter:: await_suspend(std::coroutine_handle<> h)
{
    // 修复2+5：用 find 查找，不用 conns[fd]（可能创建幽灵条目）
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end() || it->second->state.closed)
        return false; // 连接已关闭，不挂起

    auto &conn = *it->second;
    conn.session->coroutine_context.handle = h;
    conn.session->coroutine_context.state=AwaitType::READ;
    conn.session->coroutine_context.waiting=true;
   
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

    // 注册 EPOLLOUT 等待写事件
    conn.state.wantWrite = true;

    reactor->updateEvent(fd);
    return true;
}

void WriteAwaiter::await_resume()
{
}