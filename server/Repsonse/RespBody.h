#ifndef RESP_BODY_H
#define RESP_BODY_H
#include <sys/uio.h>
#include <vector>
#include <string>
#include <memory>

#include"server/SegmentPool/SegmentPool.h"

#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)
// 抽象类模版
class RespBody
{
public:
    virtual ~RespBody() = default;

    // virtual bool next(std::vector<Segment>& seg,size_t max)=0;
    // 池化内存，优化内存分配
    virtual int buildSegments(Block *block,size_t max)=0;
    // 构造发送链
    // virtual bool buildIov(std::vector<iovec> &vec,size_t maxBytes) = 0;
    // 消费发送量
    virtual void consume(size_t bytes) = 0;
    // 是否结束
    virtual bool finished() const = 0;
    // 表示剩余量
    virtual size_t remain() const =0;
    
    virtual void finish(){}
    // 判断是否需要发送file,通过共享指针的数量来判断
    // 发送文件函数
    virtual ssize_t sendFile(int fd){
        return -1;
    }
    // 使用的内存大小
    virtual size_t memoryUsage()const=0;

    // 使用mmap
    virtual bool useSendfile()const{
        return false;
    }
    
    virtual bool Chunked()const{
        return false;
    }
};
using RespBodyPtr=std::shared_ptr<RespBody>;


#endif