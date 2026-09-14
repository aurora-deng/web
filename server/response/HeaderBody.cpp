// =============================================================================
// 文件名：HeaderBody.cpp
// 职责比喻：响应头响应体实现 —— 把 HTTP 头部字符串"切一段"发给发送器
//
// 【整体比喻】
// 本文件实现 HeaderBody 的取货流程，与 StringBody 几乎一模一样：
// 响应头字符串躺在 Buffer 里，发送器来取货时从 offset 位置切出一段，
// 发出去后 consume 推进 offset，直到 offset 追上末尾。
// 唯一多了个 append() 方法，用于把头部字符串逐行追加到 Buffer。
//
// 关键技术点（初学者重点理解）：
//   1. 零拷贝：Segment.data 指向 Buffer 内部地址。
//   2. BUFFER 类型：标记为普通内存段（与 CONST/mmap 区分）。
//   3. 池化归还：析构时归还 Buffer。
// =============================================================================
#include "HeaderBody.h"

/**
 * @brief 析构：归还借来的 Buffer 给对象池
 * 【设计动机】Buffer 是池化资源，必须归还而非 delete，否则池会枯竭
 */
HeaderBody::~HeaderBody()
{
    if (buffer_)
    {
        BufferPoll::instance().release(buffer_);
    }
}

/**
 * @brief 追加头部字符串到 Buffer
 * @param s 待追加的头部片段（如 "HTTP/1.1 200 OK\r\n" 或 "Content-Length: 42\r\n"）
 * 【设计动机】响应头由多行组成，分多次 append 拼接，避免一次性构造大字符串
 */
void HeaderBody::append(const std::string &s)
{
    buffer_->append(s.data(), s.size());
}

/**
 * @brief 从当前偏移切出一段发送数据
 * @param block 段池块
 * @param max 本次最多取的字节数
 * @return 1 成功切出一段；0 已发完
 *
 * 【实现要点】与 StringBody::buildSegments 几乎相同：
 * 1. 计算剩余量 = 总长 - offset；
 * 2. 取 min(剩余, max)；
 * 3. Segment 指向 Buffer 内部地址，类型为 BUFFER。
 */
int HeaderBody::buildSegments(Block *block, size_t max)
{
    auto remain = buffer_->readableBytes() - offset;

    if (remain == 0)
        return 0;   // 已发完
    auto n = std::min(remain, max);
    auto &seg = block->segs[block->idx++];
    seg.type = Segment::BUFFER;                  // BUFFER 类型：普通内存段
    seg.data = buffer_->peek() + offset;         // 借用 Buffer 内部地址，零拷贝
    seg.len = n;
    return 1;
}

/**
 * @brief 消费已发送字节数，推进 offset
 * @param bytes 发送器确认已写出的字节数
 */
void HeaderBody::consume(size_t bytes)
{
    offset+=bytes;
}

/**
 * @brief 是否发送完毕
 * @return offset 到达数据末尾返回 true
 */
bool HeaderBody::finished() const
{
    return offset>=buffer_->readableBytes();
}

/**
 * @brief 剩余未发送字节数
 */
size_t HeaderBody::remain() const
{
    return buffer_->readableBytes()-offset;
}

/**
 * @brief HeaderBody 不支持 sendfile（数据在内存）
 * @return 固定 -1
 */
ssize_t HeaderBody::sendFile(int, size_t)
{
    return -1;
}

/**
 * @brief 统计内存占用
 * @return Buffer 可读字节数
 */
size_t HeaderBody::memoryUsage() const
{
    if(!buffer_)return 0;
    return buffer_->readableBytes();
}
