// =============================================================================
// 文件名：RespBody.h
// 职责比喻：响应体抽象基类 —— HTTP 响应内容的"包装盒接口"
//
// 【整体比喻】
// 想象你要寄快递（发送 HTTP 响应给客户端）。响应内容千差万别：可能是一段文字、
// 一个大文件、或者边生成边发的流式数据。RespBody 就是所有"包装盒"的统一规格：
// 不管盒子里装的是字符串、文件还是分块流，对外都提供同样的"取货接口"
// （buildSegments 取一段数据 / consume 确认已发出多少 / finished 是否发完）。
// 这样 TransportWriter（在 writerLoop 内驱动）只需要学一套取货流程，就能搬运任何类型的响应内容。
//
// 【多态家族】
//   RespBody（抽象基类，本文件）
//     ├─ StringBody   ：内存里的字符串，适合小响应（如 JSON、错误页）
//     ├─ FileBody     ：磁盘文件，支持 mmap 零拷贝 / sendfile 内核搬运
//     ├─ ChunkedBody  ：分块传输，不知道总长度时边生成边发
//     └─ HeaderBody   ：响应头数据，先发头部再发体
//
// 关键技术点（初学者重点理解）：
//   1. 段式发送（Segment）：响应内容不一次性拷贝，而是切成 Segment 段交给发送器，
//      配合 SegmentPool 内存池，避免大块内存分配，支持 writev 聚集写。
//   2. 流式消费（consume/remain）：发送器每发出 N 字节就调用 consume(N) 推进偏移，
//      下次 buildSegments 从新位置继续，实现"部分发送→续传"的非阻塞 I/O 模型。
//   3. 策略选择（useSendfile/Chunked）：基类提供默认实现，子类按需 override，
//      让 TransportWriter 知道该走 sendfile 内核零拷贝路径，还是分块传输路径。
//   4. 池化复用：响应体通常配合对象池使用，减少频繁 new/delete 的开销。
// =============================================================================
#ifndef RESP_BODY_H
#define RESP_BODY_H
#include <sys/uio.h>
#include <vector>
#include <string>
#include <memory>

#include"server/SegmentPool/SegmentPool.h"
#include "server/common/Units.h"

// =============================================================================
// 抽象类模版 RespBody：所有响应体类型的统一父类
// =============================================================================
// 【RespBody 通俗解释】
// 这是一个"纯接口"：它定义了 TransportWriter 取数据的标准流程，但自己不存任何数据。
// 子类（StringBody/FileBody/...）各自决定数据从哪来、怎么切。
// 只要子类实现这 6 个核心虚函数，就能无缝接入发送器。
// =============================================================================
class RespBody
{
public:
    // 虚析构：基类指针销毁时能正确调用子类析构，避免内存泄漏
    virtual ~RespBody() = default;

    // virtual bool next(std::vector<Segment>& seg,size_t max)=0;
    /**
     * @brief 从当前偏移构造一段发送数据（核心接口）
     * @param block 段池中的块，函数会把数据段写入 block->segs
     * @param max 本次最多取多少字节，由发送器按 writev 能力限定
     * @return 本次填入的段数（0 表示已发完）
     *
     * 【设计动机】采用"按需切片"而非"一次性拷贝"：
     * 发送器根据 socket 缓冲剩余容量决定 max，避免取出的数据发不出去又得缓存。
     * 段直接借用底层内存地址（buffer/mmap），零拷贝。
     */
    // 池化内存，优化内存分配
    virtual int buildSegments(Block *block,size_t max)=0;
    // 构造发送链
    // virtual bool buildIov(std::vector<iovec> &vec,size_t maxBytes) = 0;

    /**
     * @brief 消费已发出的字节数，推进内部偏移
     * @param bytes 发送器确认内核已写出的字节数
     *
     * 【设计动机】非阻塞 write 可能只发出部分数据，consume 记录"已发到哪"，
     * 下次 buildSegments 从新偏移继续，保证不重发、不漏发。
     */
    virtual void consume(size_t bytes) = 0;

    /**
     * @brief 是否已全部发送完毕
     * @return true 表示可以释放本响应体
     */
    // 是否结束
    virtual bool finished() const = 0;

    /**
     * @brief 剩余未发送字节数
     * @return 尚未发出的字节数，用于流量控制与日志
     */
    // 表示剩余量
    virtual size_t remain() const =0;

    /**
     * @brief 标记响应体完成（用于 ChunkedBody 追加结束块）
     * 默认空实现：StringBody/FileBody 等有限长度响应体不需要主动 finish
     */
    virtual void finish(){}

    /**
     * @brief 判断是否需要发送file,通过共享指针的数量来判断
     * 发送文件函数
     *
     * 【sendfile 通俗解释】
     * sendfile 是 Linux 系统调用，直接在内核态把文件内容搬到 socket，
     * 不经过用户态内存拷贝，比 read+write 快很多。
     * 默认返回 -1 表示不支持；FileBody 会 override 走真正的 sendfile 路径。
     * @param fd 目标 socket 描述符
     * @param maxBytes 本轮最多允许写出的字节数，用于 Reactor 公平调度
     * @return >0 已发送字节数；-2 表示 EAGAIN 需重试；-1 表示错误或不支持
     */
    virtual ssize_t sendFile(int, size_t){
        return -1;
    }
    // 使用的内存大小（用于统计/限流，StringBody 返回字符串长度，mmap 模式返回 0）
    virtual size_t memoryUsage()const=0;

    /**
     * @brief 是否使用 sendfile 零拷贝路径
     * @return 默认 false；FileBody 在 mmap 失效时返回 true 降级走 sendfile
     *
     * 【useSendfile 通俗解释】
     * 发送器据此分派：返回 true 就调 sendFile() 走内核零拷贝，
     * 返回 false 就走 buildSegments() 走用户态段式发送。
     */
    // 使用mmap
    virtual bool useSendfile()const{
        return false;
    }

    /**
     * @brief 是否为分块传输（HTTP chunked）
     * @return 默认 false；ChunkedBody override 返回 true
     *
     * 【Chunked 通俗解释】
     * 分块传输时响应头没有 Content-Length，用"长度\r\n数据\r\n"格式边发边写，
     * 直到发一个 0 长度块表示结束。适合动态生成、总长度未知的响应。
     */
    virtual bool Chunked()const{
        return false;
    }
};
// 响应体智能指针别名：shared_ptr 自动管理生命周期，跨协程/线程安全共享
using RespBodyPtr=std::shared_ptr<RespBody>;


#endif
