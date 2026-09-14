#pragma once
#ifndef TRANSPORT_OUTBOUND_ADMISSION_H
#define TRANSPORT_OUTBOUND_ADMISSION_H

#include "server/transport/ConnectionKey.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

/** 跨线程邮箱的准入上限；既限制整个 Reactor，也限制单个目标连接。 */
struct OutboundPostLimits
{
    std::size_t maxTasks = 16384;
    std::size_t maxBytes = 64UL * 1024UL * 1024UL;
    std::size_t maxTasksPerConnection = 256;
    std::size_t maxBytesPerConnection = 4UL * 1024UL * 1024UL;
};

/**
 * 跨线程出站邮箱的纯准入策略。它不加锁、不唤醒 eventfd，调用方负责串行访问。
 */
class OutboundAdmission
{
public:
    explicit OutboundAdmission(OutboundPostLimits limits = {})
        : limits_(limits)
    {
    }

    /** 尝试为目标连接预留一张任务和对应 wire 字节；超过任一上限返回 false。 */
    bool tryReserve(ConnectionKey key, std::size_t bytes);

    /** 当前批次被 Reactor 取走后一次性归还全部邮箱配额。 */
    void reset();

    std::size_t taskCount() const noexcept { return tasks_; }
    std::size_t wireBytes() const noexcept { return bytes_; }

private:
    struct ConnectionKeyHash
    {
        std::size_t operator()(const ConnectionKey &key) const noexcept
        {
            const auto fdHash = std::hash<int>{}(key.fd);
            const auto idHash = std::hash<uint64_t>{}(key.connId);
            return fdHash ^
                   (idHash + 0x9E3779B9U + (fdHash << 6U) + (fdHash >> 2U));
        }
    };

    struct Usage
    {
        std::size_t tasks = 0;
        std::size_t bytes = 0;
    };

    OutboundPostLimits limits_;
    std::size_t tasks_ = 0;
    std::size_t bytes_ = 0;
    std::unordered_map<ConnectionKey, Usage, ConnectionKeyHash> byConnection_;
};

#endif
