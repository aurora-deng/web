// ============================================================
// 文件名：Buffer.h
// ------------------------------------------------------------
// 【生活比喻：水管上的水桶 + 连接的体检表】
// 本文件定义两个核心结构：
//   1. ConnState —— 连接的"体检表"，记录这条 TCP 连接当前的各种状态开关
//      （是否关闭、是否因内存吃紧暂停读、是否想写、对端是否已关）。
//   2. Buffer —— 收发数据的"水桶"，底层一个 vector<char>，
//      用 readPos/writePos 双指针管理 [已读废弃区][有效数据][空闲区]。
//
// 这两个结构是整个服务器 I/O 的地基：每条连接挂一个 readBuffer（接请求数据）
// 和一个 sendBuffer（接响应数据），状态机靠 ConnState 决策走向。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. 双指针切三段：readPos/writePos 把一维数组逻辑切成三段，
//      读写只动指针不动数据，O(1) 操作。
//   2. 状态机标志位：ConnState 用一组 bool 描述连接生命周期，
//      SubReactor 据此决定 EPOLLIN/EPOLLOUT 注册与协程唤醒。
//   3. 零拷贝查找接口：peek()/beginWrite() 暴露原始指针，
//      findCRLF/findCRLFCRLF 原地扫描 HTTP 分隔符，避免复制成 string。
//   4. 半连接处理：peerClosed 标记对端写关闭，本端仍可发完响应再关，
//      实现"优雅关闭"而非粗暴 reset。
// ============================================================
#ifndef _BUFFER_H
#define _BUFFER_H
#include <vector>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <string_view>

/**
 * @brief 连接状态机：记录一条 TCP 连接的运行时状态开关
 *
 * 【状态机 通俗解释】
 * 想象连接是个病人，ConnState 就是病历上的几项指标，
 * SubReactor（主护士）每次巡视看这些指标决定下一步：
 * 该读、该写、该挂起、还是该送走（关闭）。
 *
 * 这些 bool 会被多个线程/协程读写，靠 reactor 的事件串行化保证安全。
 */
// 链接状态机
struct ConnState
{
    bool closed = false;
    // bool wantRead=true;
    bool pauseByMemory = false; // readBuffer 可读数据超限，暂停读
    bool wantWrite = false;
    bool readPaused = false; // 综合读暂停标志（任一背压条件触发即为 true）
    // 对端半关闭写方向时仍可能已有一个完整请求；先完成响应，再关闭连接。
    bool peerClosed = false;
};

// 这一段其实仍然需要拷贝，不过相较于第一代其实已经节省了很多的复制，不过这还不是最优的零拷贝
// 最优的零拷贝是通过全部使用指针来实现内容判断和传输
// 之后再加上状态机的标注对应块区内容就可以实现真正意义上的零拷贝，之后再往chunked上发展

/**
 * @brief 收发数据缓冲区：服务器收发数据的"水桶"
 *
 * 【Buffer 通俗解释】
 * 网络数据像水流，socket 是水龙头，Buffer 是接水的桶。
 *   - readPos：水喝到哪了（已读位置）
 *   - writePos：水接到哪了（已写位置）
 *   - [readPos, writePos) 之间是还没处理的"有效水"
 *   - [0, readPos) 是喝过的"废水"，可被紧凑化回收
 *   - [writePos, end) 是还能接水的"空位"
 *
 * 详细实现见 Buffer.cpp。
 */
struct Buffer
{
    std::vector<char> buf;  // 桶身：底层动态数组，自动管理内存

    size_t readPos = 0;     // 读水位：下次 peek/retrieve 的起点
    size_t writePos = 0;    // 写水位：下次 beginWrite/append 的起点

    Buffer(size_t initial = 8192);

    // 可读长度
    size_t readableBytes() const;

    // 可写长度
    size_t writableBytes() const;

    // 可读长度开始指针
    const char *peek() const;
    // 可写长度开始指针
    char *beginWrite();
    const char *beginWrite() const;
    // 重置
    void retrieve(size_t len);

    // 扩容
    void ensureWrite(size_t len);
    // 追加
    void append(const char *data, size_t len);
    void append(std::string_view s);
    // 刷新数据
    void clear();
    // 零拷贝，使用指针来搜索出head
    const char *findCRLFCRLF() const;
    const char *findCRLF() const;
};

#endif
