// ==============================================================================
// 文件名：http.h
// 职责比喻：这是 HTTP 世界的"户籍登记表 + 信纸模板"。
//   就像邮局要分清"信封（请求/响应头）"和"信纸（消息体）"，本文件登记了 HTTP 通信中
//   最基础的两类"住户信息"——HttpRequest（客户端寄来的请求信封）和 HttpResponse（服务器
//   要回寄的响应信封）。所有解析器、编解码器、发送器都围绕这两个结构展开工作：
//   解析器把字节流填进 HttpRequest，业务处理完后生成 HttpResponse，发送器再把 HttpResponse
//   打包成字节流寄回客户端。
//
// 关键技术点（初学者重点理解）：
//   1. 【请求-响应模型】HTTP 是"一问一答"的协议：客户端发一个 HttpRequest，服务器回一个
//      HttpResponse。理解这一点就理解了整个 Web 服务器的主循环。
//   2. 【ParseState 三态】解析请求时只有三种结果：PARSE_OK（完整拿到）、PARSE_NEED_MORE
//      （数据没到齐，等下次）、PARSE_ERROR（格式坏了，关连接）。这是处理 TCP 半包/粘包的核心。
//   3. 【多态响应体 RespBodyPtr】响应体不只是字符串，可能是内存块、磁盘文件、分块流。用基类
//      指针 RespBodyPtr 统一管理，发送器无需关心具体类型——这就是面向对象的"多态"实战。
//   4. 【RangeInfo 断点续传】支持 Range 请求头，让浏览器能"分段下载"大文件（如视频拖动进度条）。
// ==============================================================================
#ifndef HTTP_H
#define HTTP_H
#include <string>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <deque>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include<unordered_map>
#include <fcntl.h>
#include <utility>
#include<ctime>


#include "server/buffer_pool/BufferPoll.h"
#include "server/Buffer/Buffer.h"
#include"server/SegmentPool/SegmentPool.h"
#include "server/response/RespBody.h"
#include"server/response/FileBody.h"
#include "server/common/Units.h"
#include"server/response/ChunkedBody.h"
#include"server/response/StringBody.h"
#include"server/response/HeaderBody.h"


/**
 * @brief HTTP 请求解析结果的三态枚举
 *
 * 通俗解释：就像快递员分拣包裹时判断"这箱货齐没齐"：
 *   - PARSE_OK：包裹完整，可以签收交付业务处理；
 *   - PARSE_NEED_MORE：箱子还没到齐（TCP 半包），先放着等下一车；
 *   - PARSE_ERROR：箱子摔坏了（格式非法），直接拒收关闭连接。
 *
 * 【半包/粘包 通俗解释】TCP 是"流式"传输，没有消息边界：一次 recv 可能只收到半个请求
 * （半包），也可能一次收到两个半请求（粘包）。三态机制让解析器能"按需停顿"，数据不够就
 * 等下一次，数据多了就分多次解析，保证不丢不重。
 */
enum ParseState
{
    PARSE_OK,
    PARSE_NEED_MORE,
    PARSE_ERROR
};

/**
 * @brief Range 请求的范围信息（用于断点续传/分段下载）
 *
 * 通俗解释：对应 HTTP 的 Range 请求头，告诉服务器"我只要文件的某一段"。
 * 比如视频播放器拖动进度条，会请求 "bytes=1000-2000" 这样的范围。
 *
 * 【suffix 后缀范围 通俗解释】形如 "bytes=-500" 表示"要最后 500 字节"，
 * suffix=true 标记这种"从末尾算起"的特殊形式。
 */
struct RangeInfo
{
    bool enable = false;  // 是否启用了 Range 请求

    size_t begin = 0;     // 起始字节偏移（闭区间）
    bool suffix = false;  // 是否为后缀形式 "bytes=-N"
    size_t end = 0;       // 结束字节偏移（闭区间）
};

/**
 * @brief HTTP 请求结构体——客户端寄来的"请求信封"
 *
 * 通俗解释：解析器把字节流拆解后，把每一部分填到对应字段里，就像邮局工作人员把信封上的
 * "收件人、发件人、正文"分别誊抄到登记表上。业务代码只需读这个结构，不用再碰原始字节。
 *
 * 【分层保存 path 与 raw_path 通俗解释】raw_path 保留原始 URL（含查询串），path 是去掉
 * 查询串的纯路径用于路由匹配。这样后续做 URL 解码或签名校验时，原始信息还在，不会丢失。
 */
struct HttpRequest
{
    bool valid = true;   // 请求是否合法（解析失败时置 false）

    std::string method;  // 请求方法：GET / POST / PUT / DELETE 等
    std::string raw_path;    // 原始请求目标（含 ?query 串）
    std::string path;    // 去掉查询串的纯路径，供路由匹配
    std::string query;   // 查询串（? 后面的部分，不含 ?）
    std::string version; // HTTP 版本："HTTP/1.1" 或 "HTTP/1.0"
    std::unordered_map<std::string, std::string> headers;      // 请求头键值表（key 已小写化）
    std::unordered_map<std::string, std::string> querryParams; // 查询参数表（?a=1&b=2 解析后）

    // const char *bodyData = nullptr;
    std::string bodyData; // 请求体原始字节（POST 表单、JSON 等放这里）

    RangeInfo range;     // Range 范围信息（断点续传用）

    size_t bodySize = 0; // 请求体字节数
    // 使用string_view这个是一个指针，不会复制内容
    // std::string_view body;
    void reset();        // 清空所有字段，为复用连接解析下一个请求做准备
};


/**
 * @brief HTTP 响应结构体——服务器要回寄的"响应信封"
 *
 * 通俗解释：业务 handler 处理完请求后，把结果填进这个结构：状态码（200 成功 / 404 找不到）、
 * 响应头、响应体。然后交给 TransportWriter（在 writerLoop 内用 writev/sendfile）打包成字节流发回客户端。
 *
 * 【多态响应体 通俗解释】响应体 body 是基类指针 RespBodyPtr，实际可能是：
 *   - StringBody：内存中的字符串/字节块（小响应、JSON）；
 *   - FileBody：磁盘文件，支持 mmap 或 sendfile 零拷贝（大文件下载）；
 *   - ChunkedBody：分块传输，边产生边发（流式响应、SSE）。
 *   发送器只调基类接口，不关心具体类型，这就是多态的威力。
 *
 * 【HeaderBody_ 与 body 分离 通俗解释】响应头和响应体分开存放，因为发送时必须先发头再发体，
 * 且头通常一次性构建完毕，体可能流式产生。分开存放便于发送器独立管理两者的发送进度。
 */
struct HttpResponse
{
    int status = 200;   // HTTP 状态码：200/304/404/416/500 等

    std::string statusText = "OK";  // 状态码对应的文本，如 "OK"、"Not Found"

    std::unordered_map<std::string, std::string> headers; // 业务自定义响应头

    // 使用多态的基类实现
    RespBodyPtr HeaderBody_=nullptr; // 序列化后的响应头字节流（buildHeader 生成）
    RespBodyPtr body = nullptr;      // 响应体（StringBody/FileBody/ChunkedBody 之一）
    bool keepAlive = true;   // 是否保持连接（Keep-Alive），false 则发完就关连接

    // 介入chunked
    bool chunked = false;    // 是否启用分块传输（chunked Transfer-Encoding）
    // 使用流式
    // StreamQueuePtr stream;

    HttpResponse();

    /**
     * @brief 发送静态文件（支持 sendfile 零拷贝、mmap、Range、缓存协商）
     * @param path 文件磁盘路径
     * @param req  对应的请求（用于读取 If-None-Match 等条件头）
     * @param range 客户端请求的字节范围
     * @return true 成功装入响应体；false 文件不存在或读取失败
     * @note 会根据情况设置 304（未修改）/206（部分内容）/416（范围越界）等状态
     */
    bool sendfile(const std::string &path,const HttpRequest& req, RangeInfo &range);

    // 这个其实是拼接，这样就是实现需要copy增大消耗
    // std::string toString() const;
    std::string buildHeader() const;  // 把响应头拼成字符串（调试/旧路径用）
    void buildHeader();               // 把响应头序列化进 HeaderBody_（生产路径，零拷贝友好）
// 流式发送chunk
    void beginChunked();              // 开启分块传输模式，写入 Transfer-Encoding 头
    void writeChunk(const std::string &s); // 追加一个分块（自动加 chunk-size 前缀和 CRLF 后缀）
    void endChunked();                // 结束分块流（写入终止 0 长度块）
    // 完善response函数
    void setHeader(const std::string key, std::string value); // 设置自定义响应头

    void text(const std::string &s);  // 设置纯文本响应体（Content-Type: text/plain）

    void html(const std::string &s);  // 设置 HTML 响应体（Content-Type: text/html）

    void json(const std::string &s);  // 设置 JSON 响应体（Content-Type: application/json）
    static HttpResponse stock404();   // 工厂方法：生成标准 404 响应
    static HttpResponse stock416(size_t fileSize); // 工厂方法：生成 416 范围越界响应
    void reset();                     // 重置为初始状态，供对象池复用

};

// HttpRequest parse_request(const std::string& data);
// 优化，统一处理解析和删除，事项解析删除一体化，提高效率

ParseState try_parse_request(Buffer &buff, HttpRequest &req);

#endif
