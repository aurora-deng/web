#ifndef STRING_BODY_H
#define STRING_BODY_H

#include"RespBody.h"
#include"server/Buffer/Buffer.h"

class StringBody : public RespBody
{
public:
    ~StringBody() ;                  //用于释放buffer_pool申请的
    explicit StringBody()  {
        buffer_=BufferPoll::instance().acquire();
    }
    
    bool buildIov(std::vector<iovec> &vec,size_t maxBytes) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t remain() const override;
    ssize_t sendFile(int fd) override;
    size_t memoryUsage() const override;
    std::shared_ptr<Buffer> buffer_;

private:
    size_t offset_ = 0;
};


#endif