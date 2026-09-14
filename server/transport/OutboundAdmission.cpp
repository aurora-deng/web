#include "OutboundAdmission.h"

bool OutboundAdmission::tryReserve(ConnectionKey key, std::size_t bytes)
{
    if (!key)
        return false;

    const auto usageIt = byConnection_.find(key);
    const Usage usage = usageIt == byConnection_.end()
        ? Usage{}
        : usageIt->second;

    const bool globalTaskFull = tasks_ >= limits_.maxTasks;
    const bool targetTaskFull = usage.tasks >= limits_.maxTasksPerConnection;
    // 先判断单项，再做减法，避免 size_t 相加溢出。
    const bool globalBytesFull =
        bytes > limits_.maxBytes ||
        bytes_ > limits_.maxBytes - bytes;
    const bool targetBytesFull =
        bytes > limits_.maxBytesPerConnection ||
        usage.bytes > limits_.maxBytesPerConnection - bytes;
    if (globalTaskFull || targetTaskFull ||
        globalBytesFull || targetBytesFull)
    {
        return false;
    }

    ++tasks_;
    bytes_ += bytes;
    auto &stored = byConnection_[key];
    ++stored.tasks;
    stored.bytes += bytes;
    return true;
}

void OutboundAdmission::reset()
{
    tasks_ = 0;
    bytes_ = 0;
    byConnection_.clear();
}
