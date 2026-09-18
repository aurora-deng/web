// =============================================================================
// 文件名：ReactorGroup.cpp
// 所属模块：server/Reactor —— 多 Reactor 编排层实现
//
// 【职责比喻：楼层经理班组的调度台】
// 实现 ReactorGroup 的"调度台"逻辑——start（招经理上岗）、dispatch（轮流派客）、
// postOutbound（跨楼层转交物品）、stop/join（统一打烊下班）、activeConnections
// （统计全楼住客数）。本文件本身不碰 epoll/协程，所有重活都委托给 SubReactor。
//
// 关键技术点（初学者重点理解）：
// 1. 【start 一次性 + SessionFactory 注入】start 检查 count>0 且 reactors_ 为空，
//    否则抛 logic_error。每新建一个 SubReactor 后立即 setSessionFactory(&sessionFactory_)
//    把协议升级工厂注入——这是 SubReactor 后续能创建 WebSocketSession 的关键。
//    再 setReactorIndex(i) 让 SubReactor 知道自己是第几位（便于外部按 index 跨 Reactor
//    寻址），最后 run() 开线程。
// 2. 【轮询 dispatch 无锁 O(1)】dispatch 用 next_ 游标依次把 fd 派给每个 SubReactor，
//    next_ = (next_ + 1) % size()。无锁、O(1)、负载均衡，是 main accept 线程的标准做法。
// 3. 【跨 Reactor postOutbound】按 reactorIndex 找到目标 SubReactor 并调它的 postOutbound，
//    把跨线程投递的细节（eventfd 唤醒、pending 队列、connId 校验）全部下沉给 SubReactor
//    处理。本类只做"按 index 路由"这一件事。
// 4. 【析构链 ~ReactorGroup】自动 stop()+join()，保证对象析构前所有 SubReactor 线程
//    已退出。stop/join 都幂等，多次调用安全。
// =============================================================================
#include "ReactorGroup.h"

#include "server/Executor/Executor.h"
#include "server/Route/Router.h"
#include "server/SubReactor/SubReactor.h"
#include "server/session/SessionFactory.h"

#include <stdexcept>
#include <utility>

/**
 * @brief 构造函数：仅持有三件公共资源的引用，不创建 SubReactor
 *
 * 【通俗解释】班组组建——先登记路由表、后厨线程池、协议升级工厂的引用，
 * 真正的经理（SubReactor）要等 start() 时才招进来。
 */
ReactorGroup::ReactorGroup(Router &router,
                           Executor &executor,
                           SessionFactory &sessionFactory)
    : router_(router),
      executor_(executor),
      sessionFactory_(sessionFactory)
{
}

/**
 * @brief 析构函数：统一 stop + join 全部 SubReactor
 *
 * 【通俗解释】班组散伙前先让所有经理打烊下班（stop 通知 + join 等待），
 * 确保所有 SubReactor 线程退出后才销毁本对象，避免野指针访问。
 */
ReactorGroup::~ReactorGroup()
{
    stop();
    join();
}

/**
 * @brief 一次性创建并启动 count 个 SubReactor 线程
 * @param count SubReactor 数量（必须 >0，且本组未启动过）
 *
 * 【通俗解释】
 * 一次性招 count 位楼层经理上岗，每位经理的入职流程：
 *   ①make_unique<SubReactor>(router_, executor_) 建对象（共享路由表和后厨）；
 *   ②setSessionFactory(&sessionFactory_) 把协议升级工厂派给他——后续他收到
 *     HTTP Upgrade: websocket 时就能创建 WebSocketSession；
 *   ③setReactorIndex(i) 告诉他是第几位（外部按 index 跨 Reactor 寻址用）；
 *   ④run() 开他自己的事件循环线程。
 *   重复 start 会抛 logic_error，保证"只能启动一次"。
 */
void ReactorGroup::start(size_t count)
{
    if (count == 0 || !reactors_.empty())
        throw std::logic_error("ReactorGroup requires a non-zero, one-time start");
    reactors_.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        reactors_.push_back(std::make_unique<SubReactor>(router_, executor_));
        reactors_.back()->setSessionFactory(&sessionFactory_); // 协议升级工厂注入——SubReactor 据此创建 WebSocketSession
        reactors_.back()->setTlsContext(tlsContext_);
        reactors_.back()->setReactorIndex(i);
        reactors_.back()->run();
    }
}

/**
 * @brief 轮询派发新连接给下一个 SubReactor
 * @param fd 新接受的 socket
 *
 * 【通俗解释】
 * 前台 accept 到一个新 fd，按 next_ 游标轮流派给下一位经理（addFd），再把游标拨一位。
 * 这样负载在所有经理间均匀分布，无需锁，O(1)。
 */
void ReactorGroup::dispatch(int fd, bool tls)
{
    if (reactors_.empty())
        throw std::logic_error("cannot dispatch before ReactorGroup::start");

    reactors_[next_]->addFd(fd, tls);
    next_ = (next_ + 1) % reactors_.size();
}

/**
 * @brief 跨 Reactor 投递出站任务
 * @param reactorIndex 目标 SubReactor 在组里的下标
 * @param fd 目标 socket
 * @param connId 连接 id（消费时校验）
 * @param task 出站任务
 *
 * 【通俗解释】
 * 某条连接的协程跑到别的线程（比如 Worker）想给本连接发数据时调用：按 reactorIndex
 * 找到目标 SubReactor，调它的 postOutbound——后者会把任务先存进 pending 队列并写
 * eventfd，由目标 SubReactor 自己的线程在 processPendingOutbound 里消费。这样
 * 避免直接摸目标 SubReactor 的 conns 表（线程不安全）。reactorIndex 越界直接丢弃。
 */
EnqueueResult ReactorGroup::postOutbound(size_t reactorIndex,
                                         int fd,
                                         uint64_t connId,
    OutboundTask task)
{
    if (reactorIndex >= reactors_.size())
    {
        task.complete(OutboundOutcome::Invalid);
        return EnqueueResult::Invalid;
    }
    return reactors_[reactorIndex]->postOutbound(
        fd, connId, std::move(task));
}

/**
 * @brief 通知所有 SubReactor 打烊
 *
 * 【通俗解释】给每位经理发"打烊"信号——内部是调每个 SubReactor::stop()，把 running
 * 置 false 并写 eventfd 唤醒可能阻塞在 epoll_wait 的线程。幂等，多次调用安全。
 */
void ReactorGroup::stop()
{
    for (auto &reactor : reactors_)
        reactor->stop();
}

/**
 * @brief 阻塞等待所有 SubReactor 线程退出
 *
 * 【通俗解释】等所有经理真正下班回家。通常先 stop() 再 join()，否则 join 会永久挂起。
 */
void ReactorGroup::join()
{
    for (auto &reactor : reactors_)
        reactor->join();
}

/**
 * @brief 全组活跃连接总数
 * @return 所有 SubReactor 的 activeConnections 之和
 *
 * 【通俗解释】问班组"现在所有楼层一共住了多少客人"——把每位经理的连接数加起来。
 * 用 atomic 累加，无需加锁。
 */
size_t ReactorGroup::activeConnections() const
{
    size_t total = 0;
    for (const auto &reactor : reactors_)
        total += reactor->activeConnections();
    return total;
}
