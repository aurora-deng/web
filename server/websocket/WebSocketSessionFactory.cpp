// ==============================================================================
// 文件名：WebSocketSessionFactory.cpp
// 所属模块：server/websocket —— WebSocket 会话专用装配台实现（2.0 新增）
//
// 【职责比喻：装配台的实际操作手册】
//   接 WebSocketSessionFactory.h 的装配台比喻：本文件是装配台的具体操作步骤——
//   构造时把 manager / dispatcher 引用绑好（构造函数），升级时 make_shared<WebSocketSession>
//   把这些依赖注入进新会话（createWebSocketSession）。逻辑极薄，因为"装配"本身就是
//   把依赖传进去再 new 一个对象的事——真正的复杂性在 WebSocketSession 内部。
//
// 【2.0 依赖倒置的落地】
//   本文件是 websocket 模块向 session/transport 层提供的"唯一具体实现入口"。
//   ReactorGroup 初始化时构造 WebSocketSessionFactory（绑定全局 manager / dispatcher），
//   再把工厂指针注入给每个 SubReactor。之后 SubReactor 只通过 SessionFactory* 抽象指针
//   调 createWebSocketSession，不再出现 WebSocketSessionManager / WebSocketDispatcher
//   具体类型——依赖方向单向干净。
//
// 关键技术点（初学者重点理解）：
//   1. 【make_shared 而非 new】createWebSocketSession 用 std::make_shared<WebSocketSession>
//      一次分配对象+控制块，比 new + shared_ptr 构造少一次堆分配，且缓存友好。
//   2. 【返回基类指针】返回 shared_ptr<Session> 而非 shared_ptr<WebSocketSession>，
//      让 Connection::session 多态持有，与 HttpSession 共享同一基类接口。
//   3. 【依赖注入】manager_ / dispatcher_ 在工厂构造时绑定，createWebSocketSession 时
//      注入进 WebSocketSession 构造函数——WebSocketSession 自己不 new 这些依赖，
//      全部由外部传入，便于测试和替换。
//   4. 【出站不归工厂管】工厂只负责"造会话"，装配出的 WebSocketSession 出站走
//      OutboundTask + OutboundQueue + writerLoop 体系，与工厂无关。
// ==============================================================================
#include "WebSocketSessionFactory.h"

#include "server/websocket/WebSocketSession/WebSocketSession.h"

// ---- 构造装配台：绑定全局 manager / dispatcher 引用（生命周期长于工厂） ----
WebSocketSessionFactory::WebSocketSessionFactory(
    WebSocketSessionManager &manager,
    WebSocketDispatcher &dispatcher)
    : manager_(manager), dispatcher_(dispatcher)
{
}

/**
 * @brief 创建一个 WebSocketSession（override SessionFactory::createWebSocketSession）
 * @param fd 连接的 socket fd（HTTP 升级后沿用同一 fd）
 * @param reactor 所属 SubReactor（事件循环 + 调度器 + OutboundQueue）
 * @param uid WebSocket 用户标识（0 表示匿名）
 * @return 装配好的 WebSocketSession，以 shared_ptr<Session> 基类指针返回
 *
 * 通俗解释：装配台干活了——把 fd / reactor / uid / manager_ / dispatcher_ 五个参数
 *   传给 WebSocketSession 构造函数，make_shared 造一个新会话。返回 shared_ptr<Session>
 *   是为了能直接 reset 进 Connection::session，与 HttpSession 共享同一基类接口，
 *   升级时无缝替换。
 */
std::shared_ptr<Session> WebSocketSessionFactory::createWebSocketSession(
    ConnectionKey key, SubReactor *reactor, UserId uid)
{
    // make_shared 一次分配对象+控制块；manager_ / dispatcher_ 是工厂绑定的全局引用
    return std::make_shared<WebSocketSession>(
        key, reactor, uid, &manager_, dispatcher_);
}
