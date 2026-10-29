#pragma once

#include <functional>
#include <utility>

/**
 * 协议只认识“把完整工作单交给谁”，不认识数据库或文件线程池。
 * submit 必须排队执行、不能内联调用；false 表示队列满，工作单未被接收。
 */
using HandlerSubmitter = std::function<bool(std::function<void()>)>;

/** 启动阶段的注册作用域。退出后还原默认执行器，不改变既有路由。 */
class HandlerExecutorScope final
{
public:
    HandlerExecutorScope(HandlerSubmitter &slot, HandlerSubmitter replacement)
        : slot_(slot), previous_(std::move(slot)) { slot_ = std::move(replacement); }
    ~HandlerExecutorScope() { slot_ = std::move(previous_); }
    HandlerExecutorScope(const HandlerExecutorScope &) = delete;
    HandlerExecutorScope &operator=(const HandlerExecutorScope &) = delete;
private:
    HandlerSubmitter &slot_;
    HandlerSubmitter previous_;
};
