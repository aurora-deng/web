// ==============================================================================
// 文件名：WebSocketHandshake.cpp
// 职责比喻：WebSocket 握手实现——HTTP 到 WebSocket 的"对暗号升级通道"全流程
//
// 【整体比喻】
// 客户端想建立 WebSocket 长连接，得先发一个特殊的 HTTP 请求"敲门对暗号"。本文件就是
// 对暗号升级通道的完整办事流程：
//   1. isUpgradeRequest：远远看一眼证件（GET + Upgrade 头），判断是不是冲着 WS 来的；
//   2. validate：逐项严格检查六项证件（method/version/Upgrade/Connection/Key/Version）；
//   3. computeAcceptKey：用公式 Base64(SHA1(Key+GUID)) 计算"通关印章"；
//   4. fillResponse：盖章放行，回 101 Switching Protocols。
//
// 【边界说明】本模块只负责握手阶段；连接建立后的二进制包裹收发、心跳 ping/pong 不在此处实现。
//
// ## 架构与依赖设计
// 1. 内置简易 SHA-1 哈希算法、标准 Base64 编码；
//    不依赖 OpenSSL/mbedTLS 第三方加密库，适合轻量服务、嵌入式设备学习与部署。
// 2. 哈希、编码工具放在匿名命名空间，仅当前文件内部使用，不会和其他库产生命名冲突。
//
// ## 参考协议标准
// - RFC 6455 §4.1   客户端发起握手请求规则
// - RFC 6455 §4.2.2 服务端握手应答与密钥计算公式
// - RFC 6455 §1.3   GUID 魔法字符串定义
// - FIPS 180-4      SHA-1 哈希算法规范
// - RFC 4648 §4     Base64 编码标准
// - RFC 7230        HTTP/1.1 请求头解析规则
//
// 关键技术点（初学者重点理解）：
//   1. Sec-WebSocket-Accept 公式：Base64(SHA1(Key + GUID))，GUID 是协议固定的魔法字符串
//      "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"——这个字符串写死在 RFC 里，不能改。
//   2. 多重校验：method/version/Upgrade/Connection/Key/Version 六项缺一不可，防止伪请求。
//   3. 内置 SHA-1+Base64：不依赖 OpenSSL，匿名命名空间隔离，避免符号冲突。
//   4. 101 响应无 body：协议切换响应只含头，发出后 TCP 通道立即变成 WebSocket 帧流。
// ==============================================================================
#include "WebSocketHandshake.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
// 多重验证（HTTP 请求头规则校验） + 双方核对同一套公式算出的哈希结果（暗号比对）
namespace {

/**
 * @brief WebSocket 协议固定字符串 GUID（RFC 6455 §1.3 强制规定，不能修改）
 *
 * 密钥计算规则：
 *   Sec-WebSocket-Accept = Base64(SHA1(客户端 Sec-WebSocket-Key + 本 GUID))
 *
 * 通俗解释：这个 GUID 就像江湖上固定的一句"切口"——双方都把它背下来，
 *   客户端把自己的随机 Key 和这句切口拼在一起做 SHA1，服务端也用同一公式算，
 *   算出来的"指纹"对得上，说明双方都懂 WebSocket 协议，可以升级通道。
 *
 * @note 新手注意：这个机制**不是密码登录鉴权**
 *   作用：用来区分真正支持 WebSocket 的服务端，避免老旧 HTTP 代理错误拦截长连接。
 *   GUID 来源：RFC 6455 §1.3 直接写死的魔法字符串，全世界所有 WebSocket 实现都用同一个。
 */
constexpr const char *kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/**
 * @brief 生成字符串小写副本，原始字符串不会被修改
 * @param s 原始字符串
 * @return 全部转为小写的新字符串
 * @note HTTP 请求头内容大小写不敏感，判断字段时统一转小写方便匹配
 */
std::string toLowerCopy(std::string s)
{
    for (char &c : s)
    {
        // tolower 需要无符号字符，防止中文/特殊字符触发异常
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/**
 * @brief 解析用逗号隔开的请求头内容，判断里面是否包含指定关键词
 * @param value 请求头原始内容，例如："keep-alive, Upgrade"
 * @param token 需要查找的关键词
 * @return 找到关键词返回 true，找不到返回 false
 * @note 自动忽略前后多余空格；匹配不区分大小写，适配 Connection、Upgrade 请求头
 */
bool headerContainsToken(const std::string &value, const char *token)
{
    const std::string lower = toLowerCopy(value);
    const std::string needle = toLowerCopy(token);
    size_t start = 0;

    // 循环分割逗号，逐个取出片段对比
    while (start <= lower.size())
    {
        size_t end = lower.find(',', start);
        if (end == std::string::npos)
            end = lower.size();

        // 去掉片段前面多余空格
        size_t a = start;
        while (a < end && std::isspace(static_cast<unsigned char>(lower[a])))
            ++a;
        // 去掉片段末尾多余空格
        size_t b = end;
        while (b > a && std::isspace(static_cast<unsigned char>(lower[b - 1])))
            --b;

        // 区间有效，匹配成功
        if (b > a && lower.compare(a, b - a, needle) == 0)
            return true;
        if (end == lower.size())
            break;
        start = end + 1;
    }
    return false;
}

/**
 * @brief 在请求头 map 中查找指定字段
 * @param req HTTP 请求结构体
 * @param name 请求头名称（大小写严格匹配）
 * @return 找到返回字符串指针；未找到返回 nullptr
 * @warning 返回的指针依赖 req 对象，请求销毁后指针不能继续使用
 */
const std::string *findHeader(const HttpRequest &req, const char *name)
{
    auto it = req.headers.find(name);
    if (it == req.headers.end())
        return nullptr;
    return &it->second;
}

/**
 * @brief 简易流式 SHA1 哈希算法实现
 *
 * 通俗解释：输入一大段文字，SHA1 算出一个独一无二的"指纹"（20 字节），
 *   用来判定允许升级为 WebSocket 连接的凭证条件之一。
 *
 * 使用方式：多次 update 添加数据 → 调用 digest() 得到最终哈希结果
 *
 * @warning 学习用途：SHA1 已经存在安全缺陷，本代码仅满足 WebSocket 握手需求，
 *          不要用来做密码加密、重要数据防篡改等安全场景。
 */
class Sha1
{
public:
    /**
     * @brief 初始化重置哈希器
     */
    Sha1() { reset(); }

    /**
     * @brief 向哈希器添加一段二进制数据
     * @param data 数据缓冲区
     * @param len 数据字节长度
     */
    void update(const uint8_t *data, size_t len)
    {
        for (size_t i = 0; i < len; ++i)
        {
            block_[blockSize_++] = data[i];
            // 缓冲区存满 64 字节(512bit)，执行一轮哈希压缩计算
            if (blockSize_ == 64)
            {
                transform();
                bitLen_ += 512; // 累计已处理比特数量
                blockSize_ = 0; // 清空缓冲区，接收下一批数据
            }
        }
    }

    /**
     * @brief 重载：直接传入字符串进行哈希
     */
    void update(const std::string &s)
    {
        update(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    /**
     * @brief 结束输入，完成收尾填充，输出最终 20 字节哈希结果
     * @return SHA1 固定 20 字节二进制摘要（网络大端序）
     *
     * 新手科普：哈希算法最后必须填充数据，满足协议格式要求，分为三步
     *   1. 添加标记位 0x80
     *   2. 填充若干个 0
     *   3. 在最后 8 字节写入原始数据总长度（比特数）
     */
    std::array<uint8_t, 20> digest()
    {
        // 统计原始数据一共多少比特
        uint64_t totalBits = bitLen_ + blockSize_ * 8ULL;
        block_[blockSize_++] = 0x80; // 固定起始填充标记

        // 剩余空间放不下 8 字节长度，先把当前缓冲区填满计算一轮
        if (blockSize_ > 56)
        {
            while (blockSize_ < 64)
                block_[blockSize_++] = 0;
            transform();
            blockSize_ = 0;
        }
        // 持续补 0，预留最后的 8 字节用来存放数据长度
        while (blockSize_ < 56)
            block_[blockSize_++] = 0;

        // 将总比特数以大端序存入缓冲区尾部（网络传输统一大端）
        for (int i = 7; i >= 0; --i)
            block_[blockSize_++] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xff);
        transform();

        // 把 5 个 32 位哈希状态变量，转换成连续 20 字节结果
        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i)
        {
            out[i * 4]     = static_cast<uint8_t>((state_[i] >> 24) & 0xff); // 最高位字节
            out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xff);         // 最低位字节
        }
        return out;
    }

private:
    /**
     * @brief 重置哈希器，初始化 SHA1 标准初始值
     * @note 每次计算新哈希前都需要调用重置，清除上一次计算残留数据
     */
    void reset()
    {
        state_[0] = 0x67452301u;
        state_[1] = 0xEFCDAB89u;
        state_[2] = 0x98BADCFEu;
        state_[3] = 0x10325476u;
        state_[4] = 0xC3D2E1F0u;
        bitLen_ = 0;
        blockSize_ = 0;
    }

    /**
     * @brief 32 位无符号整数循环左移
     * @param v 待移位的数值
     * @param n 左移位数
     * @return 循环左移后的结果
     * @note 举例：rol(0b1000..., 1) → 最高位移到最低位。SHA1 内部最基础的运算单元
     */
    [[nodiscard]] static uint32_t rol(uint32_t v, uint32_t n)
    {
        return (v << n) | (v >> (32 - n));
    }

    /**
     * @brief SHA1 核心计算函数：处理一整块 64 字节数据
     * @note 流程：1.数据拆分 2.消息扩展 3.80 轮循环迭代更新哈希值
     */
    void transform()
    {
        uint32_t m[80];
        // ---- 第一步：把 64 字节原始数据拆成 16 个 32 位整数（大端） ----
        for (int i = 0; i < 16; ++i)
        {
            m[i] = (static_cast<uint32_t>(block_[i * 4]) << 24) |
                   (static_cast<uint32_t>(block_[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block_[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block_[i * 4 + 3]);
        }
        // ---- 第二步：数据扩展，把 16 个数字扩展成 80 个数字 ----
        for (int i = 16; i < 80; ++i)
            m[i] = rol(m[i - 3] ^ m[i - 8] ^ m[i - 14] ^ m[i - 16], 1);

        // 复制当前哈希状态，用于本轮迭代计算
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        // ---- 第三步：连续 80 轮循环运算 ----
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            // SHA1 分为 4 个阶段，每个阶段使用不同计算公式和常量
            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }

            // 根据 SHA1 公式更新临时变量
            uint32_t temp = rol(a, 5) + f + e + k + m[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        // 将本轮计算结果叠加到全局哈希状态
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    uint32_t state_[5]{};    ///< SHA1 5 个哈希状态变量
    uint64_t bitLen_ = 0;    ///< 累计已经处理的数据比特数量
    uint8_t block_[64]{};   ///< 64 字节数据缓冲区（单次计算块大小）
    size_t blockSize_ = 0;  ///< 当前缓冲区已经存放的字节数
};

/**
 * @brief 标准 Base64 编码实现（RFC 4648）
 *
 * 通俗解释：Base64 把二进制每 3 字节(24bit)切分为 4 个 6bit 数字，映射到可见字符；
 *   末尾不足 3 字节时，使用等号填充补齐长度。Sec-WebSocket-Accept 必须是 SHA1 结果
 *   再做 Base64 编码后的字符串，这样才能放进 HTTP 响应头里安全传输。
 *
 * @param data 待编码二进制数据
 * @param len 数据长度
 * @return Base64 字符串，不足 4 的倍数时自动添加 '=' 填充符
 * @note WebSocket 协议硬性要求使用 +/ 字母表；不要换成 URL 专用的 -_ 版本！
 */
std::string base64Encode(const uint8_t *data, size_t len)
{
    // Base64 标准字符映射表
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    // 提前分配足够内存，避免字符串频繁扩容
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;

    // 优先处理完整的 3 字节一组数据
    while (i + 2 < len)
    {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 63]);      // 取出最高 6bit
        out.push_back(kTable[(n >> 12) & 63]);      // 取出次高 6bit
        out.push_back(kTable[(n >> 6) & 63]);       // 取出第三高 6bit
        out.push_back(kTable[n & 63]);              // 取出最低 6bit
        i += 3;
    }

    // 处理末尾不够 3 字节的剩余数据，补充填充符
    if (i < len)
    {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 63]);
        if (i + 1 < len)
        {
            // 剩余 2 字节，末尾 1 个等号填充
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back(kTable[(n >> 6) & 63]);
            out.push_back('=');
        }
        else
        {
            // 只剩 1 字节，末尾 2 个等号填充
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

} // namespace

/**
 * @brief 快速粗略判断：这个 HTTP 请求有没有可能是 WebSocket 握手请求
 * @param req 收到的完整 HTTP 请求
 * @return 具备 WebSocket 特征返回 true，普通 HTTP 返回 false
 * @note 仅用于路由快速分流筛查！不做完整合法性校验，建立连接前必须调用 validate()
 */
bool WebSocketHandshake::isUpgradeRequest(const HttpRequest &req)
{
    // WebSocket 协议规定：握手请求必须是 GET
    if (req.method != "GET")
        return false;

    const auto *upgrade = findHeader(req, "upgrade");
    if (!upgrade || !headerContainsToken(*upgrade, "websocket"))
        return false;

    const auto *connection = findHeader(req, "connection");
    if (!connection || !headerContainsToken(*connection, "upgrade"))
        return false;

    // 请求必须携带客户端随机密钥 Sec-WebSocket-Key
    return findHeader(req, "sec-websocket-key") != nullptr;
}

/**
 * @brief 严格按照 WebSocket 协议校验握手请求全部条件，并且计算应答密钥
 * @param req HTTP 握手请求
 * @return Result 结果结构体：ok 标记是否合法；失败附带错误信息；成功存放 Accept 密钥
 * @note 校验失败时，上层代码需要向客户端返回 HTTP 400 Bad Request
 */
WebSocketHandshake::Result WebSocketHandshake::validate(const HttpRequest &req)
{
    Result r;
    // ---- 第 1 项：WebSocket 握手强制使用 GET 请求 ----
    if (req.method != "GET")
    {
        r.failReason = "method must be GET";
        return r;
    }
    // ---- 第 2 项：协议切换机制需要 HTTP/1.1，老旧 HTTP/1.0 不支持 ----
    if (req.version != "HTTP/1.1")
    {
        r.failReason = "HTTP/1.1 required";
        return r;
    }
    // ---- 第 3 项：Upgrade 头必须包含 websocket ----
    const auto *upgrade = findHeader(req, "upgrade");
    if (!upgrade || !headerContainsToken(*upgrade, "websocket"))
    {
        r.failReason = "Upgrade: websocket required";
        return r;
    }
    // ---- 第 4 项：Connection 头必须包含 upgrade ----
    const auto *connection = findHeader(req, "connection");
    if (!connection || !headerContainsToken(*connection, "upgrade"))
    {
        r.failReason = "Connection: Upgrade required";
        return r;
    }
    // ---- 第 5 项：必须携带非空的 Sec-WebSocket-Key ----
    const auto *key = findHeader(req, "sec-websocket-key");
    if (!key || key->empty())
    {
        r.failReason = "Sec-WebSocket-Key required";
        return r;
    }
    // ---- 第 6 项：version:13 就是 RFC 6455 正式版本，低于 13 都是过时废弃草案 ----
    const auto *version = findHeader(req, "sec-websocket-version");
    if (!version || *version != "13")
    {
        r.failReason = "Sec-WebSocket-Version must be 13";
        return r;
    }

    // ---- 所有条件校验通过，计算应答密钥 ----
    r.acceptKey = computeAcceptKey(*key);
    r.ok = true;
    return r;
}

/**
 * @brief 根据客户端传来的 Sec-WebSocket-Key，计算服务端应答 Sec-WebSocket-Accept
 * @param secWebSocketKey 客户端上传的 Base64 随机字符串
 * @return 计算完成的 Accept 字符串，直接放入响应头返回给浏览器
 *
 * 通俗解释：四步算出"通关印章"——
 *   1. 写入客户端随机 Key；
 *   2. 拼接协议固定 GUID 字符串 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"；
 *   3. 计算 SHA1 哈希得到 20 字节二进制结果（指纹）；
 *   4. 二进制结果 Base64 编码成可放进 HTTP 头的可见字符串。
 */
std::string WebSocketHandshake::computeAcceptKey(const std::string &secWebSocketKey)
{
    Sha1 sha;
    sha.update(secWebSocketKey);          // 第一步：写入客户端随机 key
    sha.update(std::string(kWsGuid));     // 第二步：拼接协议固定 GUID 字符串
    auto dig = sha.digest();              // 第三步：计算 SHA1 哈希得到 20 字节二进制结果
    return base64Encode(dig.data(), dig.size()); // 第四步：二进制结果 Base64 编码
}

/**
 * @brief 组装握手成功响应：101 Switching Protocols
 * @param resp 响应结构体，函数内部会清空原有内容重新填充
 * @param result validate 校验结果，调用前必须保证 result.ok == true
 * @warning 协议强制规则：101 状态码的响应**不能携带消息体(body)**
 *          发送这条响应之后，TCP 通道不再传输 HTTP 数据，正式切换成 WebSocket 二进制帧
 */
void WebSocketHandshake::fillResponse(HttpResponse &resp, const Result &result)
{
    resp.reset();
    resp.status = 101;                          // 状态码：切换协议
    resp.statusText = "Switching Protocols";
    resp.keepAlive = true;
    resp.body = nullptr;                        // 禁止携带 body！

    resp.setHeader("Upgrade", "websocket");
    resp.setHeader("Connection", "Upgrade");
    resp.setHeader("Sec-WebSocket-Accept", result.acceptKey);
}
