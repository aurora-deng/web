#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include<vector>
#include <mutex>
#include<memory>

#include"server/Buffer/Buffer.h"
// 段错误修复处：移除 #include "server/Repsonse/RespBody.h"，打破循环依赖
// BufferPoll.h → RespBody.h → http.h → BufferPoll.h (循环)


class BufferPoll
{
private:
    std::mutex mtx_;
    std::vector<std::shared_ptr<Buffer>>pool;
public:
    static BufferPoll& instance();
    std::shared_ptr<Buffer> acquire();
    void release(std::shared_ptr<Buffer>);
};



#endif