// ============================================================
// 文件名：Buffer.cpp
// ------------------------------------------------------------
// 【生活比喻：水管上的水桶】
// Buffer 是服务器收发数据的"水桶"。网络数据像水一样从 socket 流进来，
// 业务代码再从桶里舀水出去处理。水桶要管两件事：
//   1. 水位（可读/可写字节数）—— 知道现在有多少水能喝、还能装多少。
//   2. 防溢出（扩容/紧凑）—— 水快满了要么把桶变大，要么把已喝掉的
//      "空位"压实到桶底，腾出空间继续接水，避免反复 malloc/free 造成内存碎片。
//
// 底层用一个 std::vector<char> 当桶身，readPos/writePos 两个指针
// 像水位刻度，标记"已读到哪/已写到哪"。读写只移动指针，不搬运数据，
// 这是缓冲区能高效运转的核心。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. 读写双指针：readPos/writePos 把 buffer 一刀切三段
//      [已读废弃区][有效数据区][空闲待写区]，移动指针代替搬运数据。
//   2. 紧凑化(compact)：当尾部空间不够但加上前面已读区够用时，
//      把有效数据搬到桶底，避免无脑 resize 扩容，省内存又省拷贝。
//   3. 零拷贝查找：findCRLF/findCRLFCRLF 直接返回 vector 内部指针，
//      让 HTTP 解析器原地扫描 \r\n 分隔符，无需把数据复制到独立字符串。
//   4. 动态扩容兜底：前面+后面都不够时才 resize，按需增长不浪费。
// ============================================================

#include "Buffer.h"

/**
 * @brief 构造一个空 Buffer，预分配初始容量
 * @param initial 初始字节数，默认 8192（8KB，约一个常见 HTTP 请求大小）
 *
 * 【预分配 通俗解释】
 * 一开始就把桶做到 8KB 大，避免每来一个字节就 realloc 一次。
 * 就像开店前先把柜台摆好，客人来了直接上货，不用临时搭架子。
 * 8192 是经验值：典型 HTTP 请求头 + 一点 body 刚好放下，命中率最高。
 */
Buffer::Buffer(size_t initial)
{
    buf.resize(initial);
}

/**
 * @brief 当前可读字节数（桶里有多少水能喝）
 * @return writePos - readPos
 *
 * writePos 是"水倒到这"，readPos 是"水喝到这"，差值就是还没喝的存量。
 */
size_t Buffer::readableBytes() const
{
    return writePos - readPos;
}

/**
 * @brief 当前可写字节数（桶还能装多少水）
 * @return buf.size() - writePos
 *
 * 从 writePos 到桶底的剩余空间。注意：这不包含前面已读废弃区，
 * 那部分要靠 ensureWrite 紧凑化后才能复用。
 */
size_t Buffer::writableBytes() const
{
    return buf.size() - writePos;
}

/**
 * @brief 返回可读数据起始的只读指针（舀水的勺子口对准的位置）
 * @return buf.data() + readPos
 *
 * HTTP 解析器从这里开始读 \r\n、读 body，不复制数据，原地扫描。
 */
const char *Buffer::peek() const
{
    return buf.data() + readPos;
}

/**
 * @brief 返回可写位置起始的可写指针（倒水口对准的位置）
 * @return buf.data() + writePos
 *
 * recv/read 时把 socket 数据直接写到这里，省去中间临时缓冲。
 */
char *Buffer::beginWrite()
{
    return buf.data() + writePos;
}

/// @brief beginWrite 的 const 重载，供 const 成员函数使用
const char *Buffer::beginWrite() const
{
    return buf.data() + writePos;
}

/**
 * @brief 消费（取出）len 字节可读数据，把读指针往前推
 * @param len 本次消费的字节数
 *
 * 【retrieve 通俗解释】
 * 喝掉 len 口水，不真的擦掉数据，只把 readPos 往前挪。
 * 当 readPos 追上 writePos（水喝光了），两个指针一起归零，
 * 整个桶重新变空，下次从头开始装，避免指针一直往右漂。
 */
void Buffer::retrieve(size_t len)
{
    readPos += len;

    // 水喝光了：两指针归零，桶从头开始用
    if (readPos == writePos)
    {
        readPos = 0;
        writePos = 0;
    }
}

/**
 * @brief 确保尾部至少有 len 字节可写空间，不够就想办法腾
 * @param len 即将写入的字节数
 *
 * 这是 Buffer 的"扩容大脑"，分三档处理：
 *   1. 尾部本来就够 —— 啥都不做，最快路径。
 *   2. 尾部不够但加上前面已读区够 —— 紧凑化：把有效数据搬到桶底。
 *   3. 总共都不够 —— 只能 resize 真正扩容。
 *
 * 【为什么先紧凑再扩容？】
 * 紧凑化只是内存内搬运（std::copy），不调用 malloc；
 * resize 会触发堆重新分配 + 整段拷贝，代价高得多。
 * 先把前面被读过的"废水区"压掉，往往就能腾够空间，省一次 malloc。
 */
void Buffer::ensureWrite(size_t len)
{
    // ---- 档位 1：尾部空闲够，直接放行 ----
    if (writableBytes() >= len)
    {
        return;
    }

    // ---- 档位 2：前面已读区 + 尾部空闲 >= len，紧凑化即可 ----
    if (readPos + writableBytes() >= len)
    {
        size_t readable = readableBytes();
        // 未读取的有效数据 整体搬到缓冲区最前面，重置读位置。
        //         缓冲区原始：[已读数据][有效数据][空闲空间]
        // 下标：      0       readPos      writePos     end
        // 变为
        // [有效数据][空闲空间................]
        std::copy(buf.begin() + readPos, buf.begin() + writePos, buf.begin());
        // 更新位置,这里容易出现段错误
        // 必须先用 readable 记下长度再 copy，否则 copy 过程中 readPos 还指着旧位置，
        // 一旦搞错顺序就会野指针段错误。
        readPos = 0;
        writePos = readable;
    }
    // ---- 档位 3：总空间都不够，只能真正扩容 ----
    else
    {
        buf.resize(writePos + len);
    }
}

/**
 * @brief 追加 len 字节数据到缓冲区尾部（往桶里倒水）
 * @param data 数据源指针
 * @param len  数据长度
 *
 * 先 ensureWrite 保证空间，再 memcpy 落位，最后推 writePos。
 *
 * 【为什么用 memcpy 不用 std::copy？】
 * 这里数据是 POD（char 数组），memcpy 对连续内存是最优实现，
 * 编译器会生成向量化指令；std::copy 对 char 也会优化成 memcpy，
 * 但直接写 memcpy 意图更清晰，少一层模板推导。
 */
void Buffer::append(const char *data, size_t len)
{
    ensureWrite(len);
    // std::copy(data, data + len, buf.begin() + writePos);
    memcpy(beginWrite(), data, len);
    writePos += len;
}

/// @brief string_view 重载：方便直接追加字符串视图，省去手动取 data/size
void Buffer::append(std::string_view s)
{
    append(s.data(), s.size());
}

/**
 * @brief 清空缓冲区（把桶倒空）
 *
 * 只把两个指针归零，不真的清内存。buf 的容量保留着，
 * 下次还能直接用，避免重复 malloc/free。这就是对象池复用的雏形。
 */
void Buffer::clear()
{
    readPos = 0;
    writePos = 0;
}

/**
 * @brief 在可读区中查找 HTTP 头结束标记 \r\n\r\n
 * @return 找到返回指向第一个 \r 的指针，没找到返回 nullptr
 *
 * 【\r\n\r\n 通俗解释】
 * HTTP 协议规定：请求头/响应头以 \r\n\r\n 结尾（两个回车换行）。
 * 找到它就知道"头结束、body 开始"的位置，可以切分解析。
 *
 * 【零拷贝查找】
 * 不把 buffer 复制成 string 再 find，而是直接用 std::search
 * 在 vector 内部内存上扫描，返回原始指针，解析器原地处理。
 */
const char *Buffer::findCRLFCRLF() const
{
    const char *begin = peek();
    const char *end = begin + readableBytes();

    const char *p = std::search(begin, end, "\r\n\r\n", "\r\n\r\n" + 4);

    if (p == end)
        return nullptr;

    return p;
}

/**
 * @brief 在可读区中查找单行结束标记 \r\n
 * @return 找到返回指向该 \r 的指针；没找到返回 beginWrite()（可写位置）
 *
 * 用于逐行扫描 HTTP 头部（每行一个 \r\n）。
 */
const char* Buffer::findCRLF() const
{
    auto it =
        std::search(
            peek(),
            beginWrite(),
            "\r\n",
            "\r\n"+2
        );

    if(it==beginWrite())
        return beginWrite();

    return it;
}
