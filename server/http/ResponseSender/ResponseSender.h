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
    SendState send(int fd, HttpResponse &resp);

    ResponseSender(SegmentPool &pool, TimerWheel &wheel) : segPool(pool), wheel(wheel) {}

private:
    SendState sendHeader(int fd, HttpResponse &resp);

    SendState sendMemoryBody(int fd, HttpResponse &resp);

    SendState sendFileBody(int fd, HttpResponse &resp);
    SegmentPool &segPool;
    TimerWheel& wheel;

};
#endif