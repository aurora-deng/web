#pragma once
#ifndef TRANSPORT_CONNECTION_KEY_H
#define TRANSPORT_CONNECTION_KEY_H

#include <cstdint>

/**
 * @brief 一次 TCP 连接的稳定身份：fd 是位置，connId 是代际号。
 *
 * fd 会在连接关闭后被操作系统复用。任何会跨越 co_await、线程队列或延迟回调的
 * 对象都必须保存完整 ConnectionKey，恢复时同时校验两项，避免旧任务误操作新连接。
 */
struct ConnectionKey
{
    int fd = -1;
    std::uint64_t connId = 0;

    constexpr explicit operator bool() const noexcept
    {
        return fd >= 0 && connId != 0;
    }

    friend bool operator==(const ConnectionKey &, const ConnectionKey &) = default;
};

#endif
