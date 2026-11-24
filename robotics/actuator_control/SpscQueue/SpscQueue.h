#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <type_traits>

namespace nebula::actuator {

// One producer, one consumer. A full queue drops the new sample.
template<class T, unsigned Capacity>
class SpscQueue {
    static_assert(Capacity > 0 && std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<unsigned>::is_always_lock_free);

public:
    bool tryPush(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) % slots_.size();
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> takeLatest() noexcept {
        auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return {};
        }
        // Both cursors are snapshots: all entries through head remain owned by this reader.
        tail = (head + slots_.size() - 1) % slots_.size();
        const auto latest = slots_[tail];
        tail_.store(head, std::memory_order_release);
        return latest;
    }

private:
    std::array<T, Capacity + 1> slots_{};
    std::atomic<unsigned> head_{0};
    std::atomic<unsigned> tail_{0};
};

}  // namespace nebula::actuator
