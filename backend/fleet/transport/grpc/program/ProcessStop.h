#pragma once

#include <atomic>
#include <csignal>

namespace nebula::fleet::process {
static_assert(std::atomic_bool::is_always_lock_free);
inline std::atomic_bool stopped{false};
inline void onInterrupt(int) { stopped.store(true, std::memory_order_relaxed); }

inline void installStopHandlers() {
    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);
}
}
