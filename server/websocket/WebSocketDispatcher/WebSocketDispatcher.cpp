// ==============================================================================
// 文件名：WebSocketDispatcher.cpp
// 职责比喻：总台话务员的"接线操作手册" —— 实现注册分机 / 转接电话
//
// 【整体比喻】
// 接 WebSocketDispatcher.h 的总台比喻：本文件是话务员的具体操作步骤——
// 怎么往分机簿里加一条（on）、怎么设默认值班（onDefault）、怎么按代号查表转接（dispatch）。
// 逻辑非常薄，因为"按 key 查 map"本身就是 O(1) 简单操作。
//
// 关键技术点（初学者重点理解）：
//   1. 【handlers_[type] = ... 自动覆盖】unordered_map::operator[] 对已存在的 key
//      会覆盖旧值，因此同一 type 重复注册时新 handler 替代旧的——符合"后注册优先"直觉。
//   2. 【std::move(handler) 转移所有权】function 包装的回调可能持有较大捕获（lambda
//      捕获的变量），用 move 避免一次拷贝。
//   3. 【dispatch 的查找顺序】先查 handlers_（精确 type 匹配）→ 再 fallback 到
//      defaultHandler_——保证"精确匹配优先于默认兜底"。
//   4. 【defaultHandler_ 的真值判断】operator bool 检查是否设置过——空的
//      std::function 调用会抛 bad_function_call，必须先判空。
// ==============================================================================
#include "WebSocketDispatcher.h"

// ---- 注册某 type 的 handler：写入分机簿 ----
void WebSocketDispatcher::on(const std::string &type, WsHandler handler)
{
    // operator[] 对已存在 key 会覆盖旧值——后注册的 handler 替代之前的
    handlers_[type] = std::move(handler);  // move 转移所有权，避免 function 拷贝
}

// ---- 注册默认 handler：兜底值班分机 ----
void WebSocketDispatcher::onDefault(WsHandler handler)
{
    defaultHandler_ = std::move(handler);  // move 转移所有权
}

// ---- 派发：按 ctx.inbound.type 查表，未命中走 default ----
bool WebSocketDispatcher::dispatch(WsMessageContext &ctx)
{
    // ---- 第一步：精确 type 匹配（O(1) 哈希查找） ----
    auto it = handlers_.find(ctx.inbound.type);
    if (it != handlers_.end())
        return it->second(ctx);  // 命中：调对应 handler，返回其 bool 结果

    // ---- 第二步：fallback 到 defaultHandler_（兜底值班） ----
    // operator bool 判空：未调过 onDefault 时 defaultHandler_ 为空，调用会抛异常
    if (defaultHandler_)
        return defaultHandler_(ctx);

    // ---- 既没命中 type，也没设 default：让上层决定（通常是 echo 回显） ----
    return false;
}
