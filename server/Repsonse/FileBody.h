#ifndef FILE_BODY_H
#define FILE_BODY_H

#include"RespBody.h"
#include"server/Buffer/Buffer.h"


class FileBody : public RespBody
{
public:
    int fd=-1;
    off_t offset=0;
    size_t remain_=0;
    std::string filePath;
    size_t filesize;
    size_t begin;
    size_t end;
    // 判断是否需要发送file,通过共享指针的数量来判断
   
    bool buildIov(std::vector<iovec> &vec,size_t) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t memoryUsage() const override;
    size_t remain()const override;
    ssize_t sendFile(int sockfd) override;

    ~FileBody();    //用来和filepath在filecache进行统一关闭使用
};
#endif