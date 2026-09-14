#include <gtest/gtest.h>

#include "server/CoroutineScheduler/CoroutineScheduler.h"
#include "server/CoroutineScheduler/Task.h"

#include <set>
#include <tuple>
#include <vector>

namespace {

Task<void> completeImmediately()
{
    co_return;
}

} // namespace

TEST(CoroutineSchedulerTest, ReapsEveryConnectionRole)
{
    CoroutineScheduler scheduler;
    std::set<CoroutineRole> completedRoles;
    scheduler.setCompletionCallback(
        [&](int fd,
            uint64_t connId,
            CoroutineRole role,
            std::coroutine_handle<>)
        {
            EXPECT_EQ(fd, 42);
            EXPECT_EQ(connId, 99U);
            completedRoles.insert(role);
        });

    auto mainTask = completeImmediately();
    auto writerTask = completeImmediately();
    scheduler.adopt(
        42,
        99,
        CoroutineRole::Main,
        mainTask.release());
    scheduler.adopt(
        42,
        99,
        CoroutineRole::Writer,
        writerTask.release());

    scheduler.runReady();

    EXPECT_EQ(scheduler.ownedCount(), 0U);
    EXPECT_EQ(
        completedRoles,
        (std::set<CoroutineRole>{
            CoroutineRole::Main,
            CoroutineRole::Writer}));
}

TEST(CoroutineSchedulerTest, KeepsFdReuseIdentityInCompletion)
{
    CoroutineScheduler scheduler;
    std::vector<uint64_t> completedConnections;
    scheduler.setCompletionCallback(
        [&](int,
            uint64_t connId,
            CoroutineRole,
            std::coroutine_handle<>)
        {
            completedConnections.push_back(connId);
        });

    auto oldConnection = completeImmediately();
    auto reusedFdConnection = completeImmediately();
    scheduler.adopt(
        7,
        1001,
        CoroutineRole::Main,
        oldConnection.release());
    scheduler.adopt(
        7,
        1002,
        CoroutineRole::Main,
        reusedFdConnection.release());
    scheduler.runReady();

    ASSERT_EQ(completedConnections.size(), 2U);
    EXPECT_NE(completedConnections[0], completedConnections[1]);
}
