#pragma once
#ifndef TRANSPORT_ENQUEUE_RESULT_H
#define TRANSPORT_ENQUEUE_RESULT_H

/**
 * 出站准入结果。Ok 只表示任务已被当前一级接收，不表示字节已经到达对端。
 */
enum class EnqueueResult
{
    Ok,
    Backpressure,
    Closed,
    Invalid
};

#endif
