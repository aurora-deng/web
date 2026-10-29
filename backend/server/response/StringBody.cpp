// =============================================================================
// 文件名：StringBody.cpp
// 职责比喻：字符串响应体实现 —— 把内存里的文字"切一段"交给快递员
//
// 【整体比喻】
// 本文件实现 StringBody 的"取货"流程。想象一段已经写好的文字躺在 Buffer 里，
// 发送器每次来取货（buildSegments），我们就从上次发到的位置（offset_）开始，
// 切出最多 max 字节的一小段，告诉发送器"这段数据在这个地址，长度这么多"。
// 发送器把这段数据 write 出去后，回来告诉我们"发出去了 N 字节"（consume），
// 我们就把 offset_ 往前挪 N。如此循环直到 offset_ 追上数据末尾（finished）。
//
// 关键技术点（初学者重点理解）：
//   1. 零拷贝：Segment.data 直接指向 Buffer 内部内存，不复制数据。
//   2. CONST 类型标记：告诉 SegmentPool 这段内存是只读常量，不会被修改。
//   3. 池化归还：析构时把 Buffer 还给 BufferPoll，避免内存池枯竭。
// =============================================================================
#include"StringBody.h"

/**
 * @brief 析构函数：归还借来的 Buffer 给对象池
 * 【设计动机】Buffer 是池化资源，必须归还而非直接释放。
 * 如果不归还，BufferPoll 会不断创建新 Buffer，最终内存膨胀。
 */
StringBody::~StringBody()
{
    if(buffer_)
    {
        BufferPoll::instance().release(buffer_);
    }
}

// bool StringBody::next(std::vector<Segment> &seg, size_t max)
// {
//     seg.clear();
//     if(finished())return false;

//     size_t n=std::min(remain(),max);
//     seg.push_back({buffer_->peek()+offset_,n});
//     return true;
// }

// bool StringBody::buildIov(std::vector<iovec> &vec, size_t maxBytes)
// {
//     if(finished())return false;

//     iovec v;
//     v.iov_base=(void*)(buffer_->peek()+offset_);
//     v.iov_len=std::min(maxBytes,remain());
//     vec.push_back(v);
//     return true;
// }

/**
 * @brief 从当前偏移切出一段发送数据
 * @param block 段池块，把 Segment 写入 block->segs[block->idx++]
 * @param max 本次最多取的字节数（受 socket 发送能力限制）
 * @return 1 表示成功切出一段；0 表示已发完
 *
 * 【实现要点】
 * 1. 先判断 finished：已发完直接返回 0，避免越界访问。
 * 2. n = min(剩余量, max)：取两者较小值，保证不超出 socket 缓冲容量。
 * 3. Segment.data 直接指向 buffer_->peek()+offset_：零拷贝，借用 Buffer 内存。
 * 4. type = CONST：标记为只读常量段，发送器不会修改它。
 */
int StringBody::buildSegments(Block *block,size_t max)
{
    if(finished())return 0;
    size_t n=std::min(remain(),max);
    Segment& seg=block->segs[block->idx++];
    seg.data=buffer_->peek()+offset_;   // 直接借用 Buffer 内部地址，零拷贝
    seg.len=n;
    seg.type=Segment::CONST;            // 标记为只读常量段
    return 1;
}

/**
 * @brief 消费已发送字节数，推进偏移
 * @param bytes 发送器确认已写出的字节数
 * 【设计动机】非阻塞 write 可能只发出一部分，consume 记录进度，
 * 下次 buildSegments 从新 offset 继续取，保证不重发不漏发。
 */
void StringBody::consume(size_t bytes)
{
    offset_+=bytes;
}

/**
 * @brief 判断是否全部发送完毕
 * @return offset_ 到达数据末尾返回 true
 */
bool StringBody::finished() const
{
    return offset_>=buffer_->readableBytes();
}

/**
 * @brief 剩余未发送字节数
 * @return 数据总长度减去已发送偏移
 */
size_t StringBody::remain() const
{
    return buffer_->readableBytes()-offset_;
}

/**
 * @brief StringBody 不支持 sendfile（数据在内存不在文件）
 * @return 固定 -1，告诉发送器走 buildSegments 路径
 */
ssize_t StringBody::sendFile(int, size_t)
{
    return -1;
}

/**
 * @brief 统计当前占用的内存大小
 * @return Buffer 中可读字节数
 * 【用途】用于内存限流：当连接积压数据过多时暂停读取，防止 OOM
 */
size_t StringBody::memoryUsage() const
{
    if(!buffer_)return 0;
    return buffer_->readableBytes();
}
