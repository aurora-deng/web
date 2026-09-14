// ==============================================================================
// 文件名：WebSocketCodec.cpp
// 职责比喻：WebSocket 编解码器实现——"翻译官"的实际工作流程（对称 HttpCodec.cpp）
//
// 【整体比喻】
// 翻译官（WebSocketCodec）一天干两件事：
//   - 上行（decode/messageFromFrame/dispatch）：拆解员交来一个"二进制包裹"（WsFrame），
//     翻译官把里面的"货物"（payload）整理成业务能懂的消息（WebSocketMessage），
//     再交给 Dispatcher 派发给具体 handler；
//   - 下行（encode*）：业务要回消息，翻译官按 RFC 6455 把消息组帧成字节串，
//     返回给 WebSocketSession 由其入 OutboundTask 队列。
//
// 【在系统中的角色】
// 对称 HttpCodec.cpp——两者都提供 decode/dispatch/encode 接口（不继承抽象基类），
// 区别在于 HttpCodec 处理 HTTP 文本协议，本文件处理 WebSocket 二进制帧协议。
// 本文件还内置了"业务消息解析"逻辑（parseInboundType），把客户端发来的 JSON 文本
// 或 @user:text 格式解析成 type/toUserId 字段，这是 HTTP 模块没有的额外职责。
//
// 关键技术点（初学者重点理解）：
//   1. 【服务端发帧不加掩码】encodeRaw 不写 mask 位——RFC 6455 §5.1 规定服务端→客户端
//      帧禁止掩码。这是和客户端发帧最大的不同，客户端必须掩码（§5.3）。
//   2. 【长度字段三档】payload <126 用 7 位内联长度；<=65535 用 126 标记+16 位扩展长度；
//      更大用 127 标记+64 位扩展长度。所有多字节长度字段均为大端序（网络序）。
//   3. 【Close 帧 payload 格式】前 2 字节是关闭码（大端），其后是可选原因字符串。
//   4. 【扁平 JSON 解析】不引入第三方 JSON 库，但完整处理字符串转义、Unicode 转义与
//      整数字段；协议只允许一层对象。复杂 JSON 或生产业务应换用成熟 JSON 库。
//   5. 【decode 后自动 reset】解析出完整帧（Ok/Closed）后立刻 reset parser，让下一帧
//      能从 Buffer 中继续解析，对上层透明。
// ==============================================================================
#include "WebSocketCodec.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketTypes/WebSocketLimits.h"
#include "server/websocket/WebSocketValidation/WebSocketValidation.h"

#include <charconv>
#include <cctype>
#include <cstring>
#include <optional>
#include <unordered_set>

namespace {

/**
 * @brief 把 opcode/fin/payload 按 RFC 6455 组帧成字节串（服务端版本，不加掩码）
 * @param opcode 帧类型（包裹类型）
 * @param fin 是否为最后一帧
 * @param payload 应用数据（货物）
 * @return 已编码的帧字节串
 *
 * 通俗解释：把"包裹类型""是否最后一箱""货物"打包成一个完整的"二进制包裹"。
 *   注意服务端发的帧不写 mask 位——RFC 6455 §5.1 明确规定服务端→客户端帧禁止掩码，
 *   只有客户端→服务端才必须掩码（§5.3）。
 *
 * @note 长度字段三档：<126 内联 7 位；<=65535 用 126+16 位；更大用 127+64 位。均大端序。
 */
std::string encodeRaw(WsOpcode opcode, bool fin, const std::string &payload)
{
    switch (opcode)
    {
    case WsOpcode::Continuation:
    case WsOpcode::Text:
    case WsOpcode::Binary:
    case WsOpcode::Close:
    case WsOpcode::Ping:
    case WsOpcode::Pong:
        break;
    default:
        return {};
    }

    const bool control = (static_cast<uint8_t>(opcode) & 0x08U) != 0;
    if (control && (!fin || payload.size() > kWsMaxControlPayloadBytes))
        return {};

    std::string out;
    const size_t n = payload.size();
    // 预留容量：2 字节基础头 + 扩展长度（0/2/8）+ payload，避免反复扩容
    out.reserve(2 + (n < 126 ? 0 : (n <= 0xFFFF ? 2 : 8)) + n);

    // ---- 第 1 字节：FIN(1) + RSV(3) + opcode(4) ----
    uint8_t b0 = static_cast<uint8_t>(opcode) & 0x0F;
    if (fin)
        b0 |= 0x80;  // FIN 位置 1 表示消息最后一帧
    out.push_back(static_cast<char>(b0));

    // ---- 第 2 字节：MASK(1) + 长度(7) ----
    // 服务端发帧 MASK 位必须为 0（RFC 6455 §5.1），所以这里不写 0x80
    if (n < 126)
    {
        // 长度直接放进低 7 位
        out.push_back(static_cast<char>(n));
    }
    else if (n <= 0xFFFF)
    {
        // 126 表示后面跟 16 位扩展长度（大端）
        out.push_back(126);
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    }
    else
    {
        // 127 表示后面跟 64 位扩展长度（大端）
        out.push_back(127);
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
    }
    // ---- payload：服务端不加掩码，直接追加原始字节 ----
    out.append(payload);
    return out;
}

/** 扁平 JSON 字符串解析辅助函数：支持标准转义与 UTF-16 surrogate pair。 */
bool appendCodePoint(std::string &out, std::uint32_t codePoint)
{
    if (codePoint <= 0x7FU)
        out.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7FFU)
    {
        out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else if (codePoint <= 0xFFFFU)
    {
        if (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
            return false;
        out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else if (codePoint <= 0x10FFFFU)
    {
        out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else
        return false;
    return true;
}

int hexDigit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool readHex4(const std::string &json,
              std::size_t &pos,
              std::uint32_t &value)
{
    if (pos + 4 > json.size())
        return false;
    value = 0;
    for (int index = 0; index < 4; ++index)
    {
        const int digit = hexDigit(json[pos++]);
        if (digit < 0)
            return false;
        value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    return true;
}

bool parseJsonStringAt(const std::string &json,
                       std::size_t &pos,
                       std::string &out)
{
    if (pos >= json.size() || json[pos] != '"')
        return false;
    ++pos;
    out.clear();
    while (pos < json.size())
    {
        const unsigned char current =
            static_cast<unsigned char>(json[pos++]);
        if (current == '"')
            return true;
        if (current < 0x20U)
            return false;
        if (current != '\\')
        {
            out.push_back(static_cast<char>(current));
            continue;
        }
        if (pos >= json.size())
            return false;
        const char escaped = json[pos++];
        switch (escaped)
        {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
        {
            std::uint32_t codePoint = 0;
            if (!readHex4(json, pos, codePoint))
                return false;
            if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
            {
                if (pos + 2 > json.size() || json[pos] != '\\' ||
                    json[pos + 1] != 'u')
                    return false;
                pos += 2;
                std::uint32_t low = 0;
                if (!readHex4(json, pos, low) ||
                    low < 0xDC00U || low > 0xDFFFU)
                    return false;
                codePoint = 0x10000U +
                    ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
            }
            if (!appendCodePoint(out, codePoint))
                return false;
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

void skipWhitespace(const std::string &json, std::size_t &pos)
{
    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
}

bool consumeLiteral(const std::string &json,
                    std::size_t &pos,
                    const char *literal)
{
    const auto length = std::strlen(literal);
    if (json.compare(pos, length, literal) != 0)
        return false;
    pos += length;
    return true;
}

bool skipJsonNumber(const std::string &json, std::size_t &pos)
{
    const auto begin = pos;
    if (pos < json.size() && json[pos] == '-')
        ++pos;
    if (pos >= json.size())
        return false;
    if (json[pos] == '0')
        ++pos;
    else if (json[pos] >= '1' && json[pos] <= '9')
    {
        while (pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[pos])))
            ++pos;
    }
    else
        return false;

    if (pos < json.size() && json[pos] == '.')
    {
        ++pos;
        const auto fractionBegin = pos;
        while (pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[pos])))
            ++pos;
        if (pos == fractionBegin)
            return false;
    }
    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E'))
    {
        ++pos;
        if (pos < json.size() && (json[pos] == '+' || json[pos] == '-'))
            ++pos;
        const auto exponentBegin = pos;
        while (pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[pos])))
            ++pos;
        if (pos == exponentBegin)
            return false;
    }
    return pos > begin;
}

bool skipFlatJsonValue(const std::string &json, std::size_t &pos)
{
    if (pos >= json.size())
        return false;
    if (json[pos] == '"')
    {
        std::string ignored;
        return parseJsonStringAt(json, pos, ignored);
    }
    if (json[pos] == '{' || json[pos] == '[')
        return false;
    if (json[pos] == 't')
        return consumeLiteral(json, pos, "true");
    if (json[pos] == 'f')
        return consumeLiteral(json, pos, "false");
    if (json[pos] == 'n')
        return consumeLiteral(json, pos, "null");
    return skipJsonNumber(json, pos);
}

/**
 * 教学协议只接收一层 JSON 对象。先完整验证，再提取字段，避免把截断 JSON、重复 key
 * 或尾随垃圾误当成一封合法业务消息。
 */
bool isValidFlatJsonObject(const std::string &json)
{
    std::size_t pos = 0;
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '{')
        return false;

    std::unordered_set<std::string> keys;
    while (true)
    {
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}')
        {
            ++pos;
            skipWhitespace(json, pos);
            return pos == json.size();
        }

        std::string key;
        if (!parseJsonStringAt(json, pos, key) || !keys.emplace(key).second)
            return false;
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos++] != ':')
            return false;
        skipWhitespace(json, pos);
        if (!skipFlatJsonValue(json, pos))
            return false;
        skipWhitespace(json, pos);
        if (pos >= json.size())
            return false;
        if (json[pos] == ',')
        {
            ++pos;
            auto next = pos;
            skipWhitespace(json, next);
            if (next >= json.size() || json[next] == '}')
                return false;
            continue;
        }
        if (json[pos] != '}')
            return false;
    }
}

std::optional<std::size_t> findJsonValue(
    const std::string &json,
    const std::string &wantedKey)
{
    std::size_t pos = 0;
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '{')
        return std::nullopt;

    while (true)
    {
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] == '}')
            return std::nullopt;
        std::string key;
        if (!parseJsonStringAt(json, pos, key))
            return std::nullopt;
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos++] != ':')
            return std::nullopt;
        skipWhitespace(json, pos);
        if (key == wantedKey)
            return pos;

        if (pos < json.size() && json[pos] == '"')
        {
            std::string ignored;
            if (!parseJsonStringAt(json, pos, ignored))
                return std::nullopt;
        }
        else
        {
            if (pos < json.size() && (json[pos] == '{' || json[pos] == '['))
                return std::nullopt; // 教学协议只允许扁平对象
            while (pos < json.size() && json[pos] != ',' && json[pos] != '}')
                ++pos;
        }
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',')
        {
            ++pos;
            continue;
        }
        return std::nullopt;
    }
}

std::optional<std::string> extractJsonString(
    const std::string &json,
    const std::string &key)
{
    auto pos = findJsonValue(json, key);
    if (!pos)
        return std::nullopt;
    std::string value;
    if (!parseJsonStringAt(json, *pos, value))
        return std::nullopt;
    return value;
}

/**
 * @brief 从 JSON 字符串中提取指定 key 对应的无符号整数值（简易实现）
 * @param json JSON 文本
 * @param key 要查找的键名
 * @return 找到返回数值；未找到或解析失败返回 0
 * @note 支持 "key":123 和 "key":"123" 两种写法；解析失败返回 0
 */
UserId extractJsonUint(const std::string &json, const std::string &key)
{
    auto found = findJsonValue(json, key);
    if (!found)
        return 0;
    std::size_t pos = *found;
    std::string value;
    if (pos < json.size() && json[pos] == '"')
    {
        if (!parseJsonStringAt(json, pos, value))
            return 0;
    }
    else
    {
        const auto begin = pos;
        while (pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[pos])))
            ++pos;
        value = json.substr(begin, pos - begin);
    }
    if (value.empty())
        return 0;
    UserId result = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} &&
                   parsed.ptr == value.data() + value.size()
               ? result
               : 0;
}

bool extractJsonBool(const std::string &json, const std::string &key)
{
    const auto found = findJsonValue(json, key);
    if (!found || json.compare(*found, 4, "true") != 0)
        return false;
    const auto end = *found + 4;
    return end == json.size() || json[end] == ',' || json[end] == '}' ||
           std::isspace(static_cast<unsigned char>(json[end]));
}

void appendJsonString(std::string &json, const std::string &value)
{
    static constexpr char hex[] = "0123456789abcdef";
    json.push_back('"');
    for (const unsigned char byte : value)
    {
        switch (byte)
        {
        case '"': json += "\\\""; break;
        case '\\': json += "\\\\"; break;
        case '\b': json += "\\b"; break;
        case '\f': json += "\\f"; break;
        case '\n': json += "\\n"; break;
        case '\r': json += "\\r"; break;
        case '\t': json += "\\t"; break;
        default:
            if (byte < 0x20U)
            {
                json += "\\u00";
                json.push_back(hex[(byte >> 4U) & 0x0FU]);
                json.push_back(hex[byte & 0x0FU]);
            }
            else
                json.push_back(static_cast<char>(byte));
        }
    }
    json.push_back('"');
}

/**
 * @brief 解析入站文本帧的 payload，推断业务消息类型（type）和目标用户（toUserId）
 * @param msg 待填充的业务消息（含原始 text，函数内填充 type/toUserId/text）
 *
 * 通俗解释：客户端发来的"货物"可能是 JSON、@user:text 或纯文本，这里把它"翻译"成
 *   业务能理解的结构化字段。支持三种格式：
 *   1. JSON：{"type":"chat","to":123,"content":"hello"} → 提取 type/to/content
 *   2. @user:text → type="chat"，toUserId=用户 id，text=消息内容
 *   3. 其他 → type="echo"（回显）
 *
 * @note 解析失败时降级为 echo，保证协议容错
 */
void parseInboundType(WebSocketMessage &msg)
{
    const auto &payload = msg.text;
    if (payload.empty())
    {
        msg.type = "echo";
        return;
    }

    // ---- 格式 1：JSON 对象 ----
    std::size_t first = 0;
    skipWhitespace(payload, first);
    if (first < payload.size() && payload[first] == '{')
    {
        if (!isValidFlatJsonObject(payload))
        {
            msg.type = "echo";
            return;
        }
        if (const auto version = extractJsonUint(payload, "v");
            version != 0 && version <= UINT32_MAX)
            msg.version = static_cast<std::uint32_t>(version);
        msg.type = extractJsonString(payload, "type").value_or("echo");
        if (msg.type.empty())
            msg.type = "echo";
        msg.messageId = extractJsonString(payload, "id").value_or("");
        msg.replyTo = extractJsonString(payload, "replyTo").value_or("");
        msg.status = extractJsonString(payload, "status").value_or("");
        msg.ackRequested = extractJsonBool(payload, "ack");
        msg.fromUserId = extractJsonUint(payload, "from");
        msg.toUserId = extractJsonUint(payload, "to");
        if (msg.toUserId == 0)
            msg.toUserId = extractJsonUint(payload, "room");
        // content / text / msg 三选一，作为消息正文
        const auto content = extractJsonString(payload, "content");
        const auto text = extractJsonString(payload, "text");
        const auto msgField = extractJsonString(payload, "msg");
        if (content)
            msg.text = *content;
        else if (text)
            msg.text = *text;
        else if (msgField)
            msg.text = *msgField;
        return;
    }

    // ---- 格式 2：@userId:text 私聊语法 ----
    if (payload[0] == '@')
    {
        const auto sep = payload.find(':');
        if (sep != std::string::npos && sep > 1)
        {
            msg.type = "chat";
            try
            {
                msg.toUserId = static_cast<UserId>(std::stoull(payload.substr(1, sep - 1)));
                msg.text = payload.substr(sep + 1);
            }
            catch (...)
            {
                msg.type = "echo";
            }
            return;
        }
    }

    // ---- 格式 3：纯文本，回显 ----
    msg.type = "echo";
}

} // namespace

/**
 * @brief 解码：委托 Parser 从 Buffer 拆出一个完整帧
 * @param buffer 连接读缓冲区
 * @param parser 该连接专属的帧解析器
 * @param out 输出帧对象
 * @return 解析结果状态
 *
 * 通俗解释：翻译官把拆解员（Parser）叫来干活，拆完一个完整包裹就让他复位（reset），
 *   准备拆下一个。如果数据不够（NeedMore）或出错（Error），不复位——下次接着当前状态拆。
 */
WsDecodeResult WebSocketCodec::decode(Buffer &buffer, WebSocketParser &parser, WsFrame &out)
{
    auto state = parser.parse(buffer, out);
    // 解析出完整帧（Ok）或对端 Close 帧（Closed）后，复位 parser 准备解析下一帧
    if (state == WsDecodeResult::Ok || state == WsDecodeResult::Closed)
        parser.reset();
    return state;
}

/**
 * @brief 把帧翻译成业务消息对象（WsFrame → WebSocketMessage）
 * @param frame 已解析的帧
 * @return 业务消息对象
 *
 * 通俗解释：把"二进制包裹"里的"货物"（payload）整理成业务能懂的消息——
 *   二进制帧直接标记 type="binary"；文本帧调用 parseInboundType 解析 JSON/@语法。
 */
WebSocketMessage WebSocketCodec::messageFromFrame(const WsFrame &frame) const
{
    WebSocketMessage msg;
    msg.opcode = frame.opcode;
    msg.text = frame.payload;
    if (frame.opcode == WsOpcode::Binary)
        msg.type = "binary";
    else
        parseInboundType(msg);
    return msg;
}

/**
 * @brief 派发：把业务消息交给 Dispatcher 路由到具体 handler
 * @param ctx 消息上下文
 * @return true 表示有 handler 处理；false 表示无 handler（上层可降级为 echo）
 */
bool WebSocketCodec::dispatch(WsMessageContext &ctx)
{
    return dispatcher_.dispatch(ctx);
}

/**
 * @brief 编码：把业务消息翻译成字节串
 * @param message 业务消息
 * @return 已编码的帧字节串
 * @note 服务端发帧不加掩码（RFC 6455 §5.1）
 */
std::string WebSocketCodec::encode(const WebSocketMessage &message) const
{
    if (message.isBinary())
        return encodeBinary(message.payload());
    return encodeText(message.payload());
}

std::string WebSocketCodec::serializeApplicationMessage(
    const WebSocketMessage &message)
{
    std::string json;
    json.reserve(message.text.size() + message.type.size() +
                 message.messageId.size() + message.replyTo.size() + 96);
    json += "{\"v\":" + std::to_string(message.version);

    auto addString = [&](const char *key, const std::string &value)
    {
        if (value.empty())
            return;
        json += ",\"";
        json += key;
        json += "\":";
        appendJsonString(json, value);
    };
    auto addUser = [&](const char *key, UserId value)
    {
        if (value == 0)
            return;
        json += ",\"";
        json += key;
        json += "\":" + std::to_string(value);
    };

    addString("type", message.type);
    addString("id", message.messageId);
    addString("replyTo", message.replyTo);
    addUser("from", message.fromUserId);
    addUser("to", message.toUserId);
    addString("status", message.status);
    if (message.ackRequested)
        json += ",\"ack\":true";
    if (!message.text.empty())
        addString("content", message.text);
    else if (message.type == "chat")
        json += ",\"content\":\"\"";
    json.push_back('}');
    return json;
}

/**
 * @brief 编码：把已有帧对象翻译成字节串
 * @param frame 帧对象
 * @return 已编码的帧字节串
 * @note 服务端发帧不加掩码
 */
std::string WebSocketCodec::encode(const WsFrame &frame)
{
    if (frame.opcode == WsOpcode::Text && frame.fin &&
        !isValidWebSocketUtf8(frame.payload))
    {
        return {};
    }
    if (frame.opcode == WsOpcode::Close)
    {
        WsCloseInfo closeInfo;
        if (parseWebSocketClosePayload(frame.payload, closeInfo) !=
            WsClosePayloadResult::Ok)
        {
            return {};
        }
    }
    return encodeRaw(frame.opcode, frame.fin, frame.payload);
}

/**
 * @brief 编码文本帧
 * @param text 文本内容
 * @param fin 是否为最后一帧
 * @return 已编码的帧字节串
 */
std::string WebSocketCodec::encodeText(const std::string &text, bool fin)
{
    // fin=true 的便捷接口代表一条完整消息；分片发送需要上层做跨帧 UTF-8 校验。
    if (fin && !isValidWebSocketUtf8(text))
        return {};
    return encodeRaw(WsOpcode::Text, fin, text);
}

/**
 * @brief 编码二进制帧
 * @param data 二进制数据
 * @param fin 是否为最后一帧
 * @return 已编码的帧字节串
 */
std::string WebSocketCodec::encodeBinary(const std::string &data, bool fin)
{
    return encodeRaw(WsOpcode::Binary, fin, data);
}

/**
 * @brief 编码关闭帧
 * @param code 关闭状态码
 * @param reason 可选原因
 * @return 已编码的帧字节串
 *
 * 通俗解释：Close 帧的"货物"有固定格式——前 2 字节是关闭码（大端），其后是原因字符串。
 *   这是 RFC 6455 §5.5.1 规定的格式。
 */
std::string WebSocketCodec::encodeClose()
{
    return encodeRaw(WsOpcode::Close, true, {});
}

std::string WebSocketCodec::encodeClose(uint16_t code, const std::string &reason)
{
    if (!isValidWebSocketCloseCode(code) ||
        reason.size() > kWsMaxControlPayloadBytes - 2 ||
        !isValidWebSocketUtf8(reason))
    {
        return {};
    }

    std::string payload;
    // 关闭码按大端序写入前 2 字节
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason);
    return encodeRaw(WsOpcode::Close, true, payload);
}

std::string WebSocketCodec::encodeClose(WsCloseCode code, const std::string &reason)
{
    return encodeClose(static_cast<uint16_t>(code), reason);
}

/**
 * @brief 编码 Ping 帧（心跳探活）
 * @param payload 可选探活数据
 * @return 已编码的帧字节串
 */
std::string WebSocketCodec::encodePing(const std::string &payload)
{
    return encodeRaw(WsOpcode::Ping, true, payload);
}

/**
 * @brief 编码 Pong 帧（回应 Ping）
 * @param payload 可选回执数据
 * @return 已编码的帧字节串
 */
std::string WebSocketCodec::encodePong(const std::string &payload)
{
    return encodeRaw(WsOpcode::Pong, true, payload);
}
