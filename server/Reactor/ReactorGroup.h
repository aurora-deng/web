// =============================================================================
// 文件名：ReactorGroup.h
// 所属模块：server/Reactor —— 多 Reactor 编排层（SubReactor 集群的总调度）
//
// 【职责比喻：楼层经理班组 / 车间集群调度】
// ReactorGroup 是一组 SubReactor 的总管——像酒店所有楼层经理组成的班组（也是车间集群
// 调度员）。它本身不直接处理任何 I/O，只负责：
//   ①按需创建若干 SubReactor（start(count)），把它们的线程跑起来；
//   ②新连接到来时用轮询（round-robin）把 fd 派给某个 SubReactor（dispatch）；
//   ③别的线程/别的 Reactor 想给某连接发数据时，按 reactorIndex 路由到对应 SubReactor
//     的 postOutbound（跨 Reactor 出站投递）；
//   ④统一 stop/join 全部 SubReactor，保证析构时干净退出；
//   ⑤持有协议无关的 Router&、HTTP Executor&、SessionFactory&，在 start 时把 SessionFactory
//     注入到每个 SubReactor，让协议升级在 SubReactor 内部就能完成，本类不出现 WebSocket
//     具体类型——这是依赖倒置的关键一环。
//
// 关键技术点（初学者重点理解）：
// 1. 【轮询 dispatch】dispatch(fd) 用 next_ 游标把新 fd 依次分给每个 SubReactor，
//    next_ = (next_ + 1) % size()。这是无锁、O(1)、负载最均衡的简单分配策略；
//    SubReactor 之间互不知道彼此，互不抢锁，各自独立事件循环。
// 2. 【SessionFactory 注入】本类只持有 SessionFactory& 抽象引用，不直接依赖
//    ProtocolSessionFactory 具体类型。start() 里对每个新建的 SubReactor 调
//    setSessionFactory(&sessionFactory_)，把协议交接能力注入下去——HTTP 首部发完后可通过
//    同一个 factory 创建 WebSocketSession 或 SseSession。
// 3. 【跨 Reactor postOutbound】某条连接的 Session 协程可能在 Worker 线程跑业务，
//    想发数据时不能直接摸目标 SubReactor 的 conns（线程不安全）。本类提供
//    postOutbound(reactorIndex, fd, connId, task)，按 reactorIndex 找到目标 SubReactor
//    并调它的 postOutbound——后者会通过 eventfd 把任务转交目标 SubReactor 自己的线程。
// 4. 【start 一次性】start(count) 检查 count>0 且 reactors_ 为空，否则抛 logic_error。
//    这是"只能启动一次"的硬约束，避免重复 start 创建多份 SubReactor 线程造成混乱。
// 5. 【析构链】~ReactorGroup 自动 stop()+join()，保证对象析构时所有 SubReactor 线程
//    都已退出，不会有野指针访问。stop/join 都幂等，多次调用安全。
// =============================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>
#include "server/transport/EnqueueResult.h"
#include "server/transport/OutboundTask.h"

class Executor;
class Router;
class SubReactor;
class SessionFactory;
class TlsContext;

/**
 * @brief Reactor 组：管理一组 SubReactor 的车间集群调度
 *
 * 【通俗解释】
 * 酒店所有楼层经理的班组——总管自己不接客，只负责：①给每位经理分配楼层（start）；
 * ②新客人来了轮流派给某位经理（dispatch）；③别的部门想给某客房送东西，按房号找到
 * 对应经理转交（postOutbound）；④打烊时统一通知所有经理下班（stop+join）。
 * 总管手里握着 Router&（路由表）、HTTP Executor&（HTTP 后厨）、SessionFactory&（协议升级工厂）
 * 三件公共资源，在 start 时把 SessionFactory 派发给每位经理，让他们能独立处理协议升级。
 *
 * @note 本类对协议无关——不出现 WebSocket 类型，只通过 SessionFactory 抽象指针解耦。
 */
class ReactorGroup
{
public:
    /**
     * @brief 构造函数：仅持有引用，不创建 SubReactor
     * @param router 路由表引用
     * @param executor 业务线程池引用
     * @param sessionFactory 协议升级工厂引用
     *
     * 【通俗解释】
     * 班组组建：先登记三件公共资源（路由表、后厨、协议升级工厂）的引用，真正的经理
     * （SubReactor）要等 start() 时才招进来。
     */
    ReactorGroup(Router &router,
                 Executor &executor,
                 SessionFactory &sessionFactory);

    /**
     * @brief 析构函数：统一 stop + join 全部 SubReactor
     *
     * 【通俗解释】班组散伙前先让所有经理打烊下班，确保线程退出后对象才析构，避免野指针。
     */
    ~ReactorGroup();

    /**
     * @brief 一次性创建并启动 count 个 SubReactor 线程
     * @param count SubReactor 数量（必须 >0，且本组未启动过）
     *
     * 【通俗解释】
     * 一次性招 count 位楼层经理上岗：①为每个经理建 SubReactor 对象（传入共享的
     * router_/executor_）；②setSessionFactory(&sessionFactory_) 把协议升级工厂派发
     * 给他；③setReactorIndex(i) 告诉他自己是第几位（用于跨 Reactor postOutbound 寻址）；
     * ④run() 开他自己的事件循环线程。重复 start 会抛 logic_error。
     */
    void start(size_t count);

    /**
     * @brief 轮询派发新连接给下一个 SubReactor
     * @param fd 新接受的 socket
     * @param tls 是否来自 HTTPS 监听端口；只传递监听来源，不解析密文
     *
     * 【通俗解释】
     * 前台 accept 到一个新 fd，按 next_ 游标轮流派给下一位经理（addFd），再把游标拨一位。
     * 这样负载在所有经理间均匀分布，无需锁，O(1)。
     */
    void dispatch(int fd, bool tls = false);
    void setTlsContext(std::shared_ptr<TlsContext> context) { tlsContext_ = std::move(context); }

    /**
     * @brief 通知所有 SubReactor 打烊
     *
     * 【通俗解释】给每位经理发"打烊"信号——内部是调每个 SubReactor::stop()，把 running
     * 置 false 并写 eventfd 唤醒可能阻塞在 epoll_wait 的线程。幂等，多次调用安全。
     */
    void stop();

    /**
     * @brief 阻塞等待所有 SubReactor 线程退出
     *
     * 【通俗解释】等所有经理真正下班回家。通常先 stop() 再 join()，否则 join 会永久挂起。
     */
    void join();

    /**
     * @brief 当前 SubReactor 数量
     * @return 已启动的 SubReactor 个数
     */
    size_t size() const { return reactors_.size(); }

    /**
     * @brief 全组活跃连接总数
     * @return 所有 SubReactor 的 activeConnections 之和
     *
     * 【通俗解释】问班组"现在所有楼层一共住了多少客人"——把每位经理的连接数加起来。
     */
    size_t activeConnections() const;

    /**
     * @brief 跨 Reactor 投递出站任务
     * @param reactorIndex 目标 SubReactor 在组里的下标
     * @param fd 目标 socket
     * @param connId 连接 id（消费时校验）
     * @param task 出站任务
     *
     * 【通俗解释】
     * 某条连接的协程跑到别的线程（比如 Worker）想给本连接发数据时调用：按 reactorIndex
     * 找到目标 SubReactor，调它的 postOutbound。后者会通过 eventfd 把任务转交目标
     * SubReactor 自己的线程处理，避免直接摸它的 conns 表。reactorIndex 越界直接丢弃。
     * @return 目标 SubReactor 的准入结果
     */
    EnqueueResult postOutbound(size_t reactorIndex,
                               int fd,
                               uint64_t connId,
                               OutboundTask task);

private:
    Router &router_;                                       // 协议无关路由表引用：所有 SubReactor 共享，按 URL 分发 handler
    Executor &executor_;                                   // HTTP Worker 池：所有 SubReactor 共用；WS 使用独立池
    SessionFactory &sessionFactory_;                       // 协议升级工厂引用：start 时注入每个 SubReactor，让其能创建 WebSocketSession 等子类
    std::vector<std::unique_ptr<SubReactor>> reactors_;    // SubReactor 持有数组：每个 unique_ptr 独占一个 SubReactor 及其线程
    size_t next_ = 0;                                      // 轮询游标：dispatch 时按 (next_+1)%size 拨动，实现 round-robin 均衡
    std::shared_ptr<TlsContext> tlsContext_;
};
