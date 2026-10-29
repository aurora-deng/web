#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace webserver::ops
{

/**
 * @brief 面向高并发观测指标的分片计数器。
 *
 * 单个全局 atomic 像只有一个收费口：所有 CPU 核都要争抢同一条缓存行。
 * StripedCounter 给每个线程稳定分配一个“收费口”，写入时只碰自己的分片，
 * Prometheus 抓取时再把全部分片求和。指标允许瞬时近似，因此这种读写取舍
 * 不影响业务正确性，却能避免热路径上的缓存行来回搬运。
 */
class StripedCounter
{
public:
    void add(std::uint64_t value = 1) noexcept
    {
        slots_[stripeIndex()].value.fetch_add(value, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t load() const noexcept
    {
        std::uint64_t total = 0;
        for (const auto &slot : slots_)
            total += slot.value.load(std::memory_order_relaxed);
        return total;
    }

private:
    static constexpr std::size_t kStripeCount = 64;

    struct alignas(64) Slot
    {
        std::atomic<std::uint64_t> value{0};
    };

    static std::size_t stripeIndex() noexcept
    {
        static std::atomic<std::size_t> next{0};
        static thread_local const std::size_t index =
            next.fetch_add(1, std::memory_order_relaxed) % kStripeCount;
        return index;
    }

    std::array<Slot, kStripeCount> slots_{};
};

} // namespace webserver::ops
