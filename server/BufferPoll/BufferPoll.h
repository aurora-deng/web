#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include<vector>
#include <mutex>
#include<memory>

#include"server/Buffer/Buffer.h"
#include"server/Repsonse/RespBody.h"


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