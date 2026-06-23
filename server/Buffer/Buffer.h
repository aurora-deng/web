#ifndef _BUFFER_H
#define _BUFFER_H
#include<vector>
#include <cstddef>
#include <algorithm>
#include <cstring>

// 链接状态机
struct ConnState
{
    bool closed = false;
    // bool wantRead=true;
    bool pauseByPipeline = false;   // 任务并发数超限，暂停提交新任务
    bool pauseByMemory = false;     // readBuffer 可读数据超限，暂停读
    bool pauseByWriteBacklog = false; // 背压修复处：pendingResponses 积压超限，暂停读
    bool wantWrite = false;
    bool readPaused = false;           // 综合读暂停标志（任一背压条件触发即为 true）
};  



// 这一段其实仍然需要拷贝，不过相较于第一代其实已经节省了很多的复制，不过这还不是最优的零拷贝
// 最优的零拷贝是通过全部使用指针来实现内容判断和传输
// 之后再加上状态机的标注对应块区内容就可以实现真正意义上的零拷贝，之后再往chunked上发展
struct Buffer{
    std::vector<char> buf;

    size_t readPos=0;
    size_t writePos=0;
    
    Buffer(size_t initial= 8192);
  
    // 可读长度
    size_t readableBytes() const;
   
    // 可写长度
    size_t writableBytes() const;
  
    // 可读长度开始指针
    const char* peek() const;
    // 可写长度开始指针
    char* beginWrite() ;
    // 重置
    void retrieve(size_t len);
    
    // 扩容
    void ensureWrite(size_t len);
    // 追加
    void append(const char* data,size_t len);
    void append(std::string_view s);
    // 刷新数据
    void clear();
    // 零拷贝，使用指针来搜索出head
    const char* findCRLFCRLF() const;
    
};

#endif