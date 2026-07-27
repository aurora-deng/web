#pragma once
#ifndef RESPONSE_SENDER_H
#define RESPONSE_SENDER_H
#include "server/http/http.h"
#include "server/timer/TimeWheel.h"
// 使用状态机来处理发送返回结果
enum SendState
{
    SEND_OK,
    SEND_AGAIN,
    SEND_CLOSED
};
class ResponseSender
{
public:
    // 发送顺序固定为 Header -> Body；HeaderBody 和 RespBody 自己保存消费位置，
    // 因此 send 可在多次 epoll 唤醒之间安全续传。
    SendState send(int fd, HttpResponse &resp);

    ResponseSender(SegmentPool &pool, TimerWheel &wheel) : segPool(pool), wheel(wheel) {}

private:
    SendState sendHeader(int fd, HttpResponse &resp);

    SendState sendMemoryBody(int fd, HttpResponse &resp);

    SendState sendFileBody(int fd, HttpResponse &resp);
    // 两者均由所属 SubReactor 独占并保证生命周期长于 Sender；引用仅借用，不产生跨线程共享。
    SegmentPool &segPool;
    TimerWheel &wheel;
};
#endif