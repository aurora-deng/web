// =============================================================================
// 文件名：StringBody.h
// 职责比喻：字符串响应体 —— 装在内存"小盒子"里的文字内容
//
// 【整体比喻】
// StringBody 是 RespBody 家族里最简单的成员，就像快递里的"信封"：
// 里面装的是一小段已经准备好的文字（HTML 片段、JSON、错误提示等）。
// 因为内容已经在内存里、长度已知，所以直接借用 Buffer 的内存地址生成 Segment，
// 发送器拿到的就是一个指针 + 长度，零拷贝地交给 writev。
//
// 适用场景：响应体较小（几 KB ~ 几百 KB），且内容已在内存中就绪。
// 不适合：大文件（应走 FileBody）、流式生成（应走 ChunkedBody）。
//
// 关键技术点（初学者重点理解）：
//   1. Buffer 池化：构造时从 BufferPoll 申请一块 Buffer，用完归还，避免反复 malloc。
//   2. offset 续传：记录已发送偏移，非阻塞 write 部分发送后可从断点继续。
//   3. 零拷贝 Segment：buildSegments 直接把 Buffer 内部指针交给发送器，不复制数据。
// =============================================================================
#ifndef STRING_BODY_H
#define STRING_BODY_H
#include<vector>
#include <sys/uio.h>
#include<algorithm>
#include<memory>


#include"RespBody.h"
#include"server/Buffer/Buffer.h"
#include"server/buffer_pool/BufferPoll.h"
#include"server/SegmentPool/SegmentPool.h"

// =============================================================================
// StringBody：内存字符串响应体，RespBody 的最简实现
// =============================================================================
// 【StringBody 通俗解释】
// 内部持有一个 Buffer（来自对象池），数据写进去后就不再变动。
// 发送时用 offset_ 记录"发到哪了"，buildSegments 从 offset 位置取一段，
// consume 把 offset 往前推，finished 判断 offset 是否到末尾。
// 整个生命周期就是"装填 → 逐段发送 → 释放回池"。
// =============================================================================
class StringBody : public RespBody
{
public:
    /**
     * @brief 析构：把借来的 Buffer 还给对象池，避免内存泄漏
     * 【设计动机】Buffer 是池化资源，必须归还而非 delete，否则池会枯竭
     */
    ~StringBody() ;

    /**
     * @brief 默认构造：从 BufferPoll 申请一个空 Buffer
     * 调用方随后通过 buffer_->append() 写入数据
     */
    explicit StringBody()  {
        buffer_=BufferPoll::instance().acquire();
    }
    // 新增：接收 shared_ptr<Buffer> 的构造函数
    /**
     * @brief 接管外部已填充好的 Buffer（避免重复拷贝）
     * @param buf 外部已写入数据的 Buffer，所有权转移给本对象
     * 【设计动机】当数据已在别处写好 Buffer 时，直接 move 进来复用，省一次拷贝
     */
    explicit StringBody(std::shared_ptr<Buffer> buf)
    {
        buffer_ = std::move(buf);
    }
    // bool next(std::vector<Segment>&seg,size_t max) override;
    int buildSegments(Block* block,size_t max) override;
    // bool buildIov(std::vector<iovec> &vec, size_t maxBytes) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t remain() const override;
    ssize_t sendFile(int fd, size_t maxBytes) override;
    size_t memoryUsage() const override;

    // 持有的数据缓冲区，shared_ptr 便于与外部共享（如 ChunkedBody 会借用它的 buffer_）
    std::shared_ptr<Buffer> buffer_;

private:
    // 已发送偏移：consume 推进，buildSegments 从此处取数据，实现非阻塞续传
    size_t offset_ = 0;
};


#endif
