#pragma once

#include <cstddef>
#include <memory>
#include <vector>


class Executor;
class HttpCodec;
class SubReactor;

// ReactorGroup 只负责 SubReactor 的生命周期和连接分发，ServerRuntime 不再了解
// 单个 Reactor 的队列、epoll 或线程细节。
// 抽象实现单reactor的功能

// 实现Reactor池化，不过使用的是链表的形式，实现一个线程多reactor，多线程n倍reactor，协程导致余下的线程都给到了reactor
class ReactorGroup
{
public:
    ReactorGroup(HttpCodec &codec, Executor &executor);
    ~ReactorGroup();

    void start(size_t count);
    void dispatch(int fd);
    void stop();
    void join();
    size_t size() const { return reactors_.size(); }
    size_t activeConnections() const;

private:
    HttpCodec &codec_;
    Executor &executor_;
    // 储存reactor的数组
    std::vector<std::unique_ptr<SubReactor>> reactors_;
    // 下一个的坐标
    size_t next_ = 0;
};
