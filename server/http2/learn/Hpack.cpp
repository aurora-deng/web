#include "Hpack.h"
#include <array>
#include <limits>
#include <stdexcept>

namespace learn_h2 {
namespace {
// RFC 7541 附录 A 的 61 项静态表：双方生来就知道，无须在连接上传整张表。
// 动态表第一项从索引 62 开始；这里保留完整静态表，方便按规范对照。
constexpr std::array<std::pair<std::string_view, std::string_view>, 61> STATIC_TABLE{{
    {":authority", ""}, {":method", "GET"}, {":method", "POST"},
    {":path", "/"}, {":path", "/index.html"}, {":scheme", "http"},
    {":scheme", "https"}, {":status", "200"}, {":status", "204"},
    {":status", "206"}, {":status", "304"}, {":status", "400"},
    {":status", "404"}, {":status", "500"}, {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"}, {"accept-language", ""},
    {"accept-ranges", ""}, {"accept", ""}, {"access-control-allow-origin", ""},
    {"age", ""}, {"allow", ""}, {"authorization", ""},
    {"cache-control", ""}, {"content-disposition", ""},
    {"content-encoding", ""}, {"content-language", ""},
    {"content-length", ""}, {"content-location", ""},
    {"content-range", ""}, {"content-type", ""}, {"cookie", ""},
    {"date", ""}, {"etag", ""}, {"expect", ""}, {"expires", ""},
    {"from", ""}, {"host", ""}, {"if-match", ""},
    {"if-modified-since", ""}, {"if-none-match", ""},
    {"if-range", ""}, {"if-unmodified-since", ""},
    {"last-modified", ""}, {"link", ""}, {"location", ""},
    {"max-forwards", ""}, {"proxy-authenticate", ""},
    {"proxy-authorization", ""}, {"range", ""}, {"referer", ""},
    {"refresh", ""}, {"retry-after", ""}, {"server", ""},
    {"set-cookie", ""}, {"strict-transport-security", ""},
    {"transfer-encoding", ""}, {"user-agent", ""}, {"vary", ""},
    {"via", ""}, {"www-authenticate", ""}
}};

// HPACK 规定动态表占用 = 名称字节数 + 值字节数 + 每项固定 32 字节开销。
size_t entry_size(const Header& h) { return h.name.size() + h.value.size() + 32; }
}

const Header& Hpack::at(size_t index) const {
    if (index == 0) throw std::runtime_error("HPACK index zero");
    if (index <= STATIC_TABLE.size()) {
        // 静态表存 string_view；这里临时组合 Header 供调用者立即复制。
        static thread_local Header value;
        const auto& pair = STATIC_TABLE[index - 1];
        value = {std::string(pair.first), std::string(pair.second)};
        return value;
    }
    // 比如索引 62：62 - 61 - 1 = 0，正好是 dynamic_.front()。
    index -= STATIC_TABLE.size() + 1;
    if (index >= dynamic_.size()) throw std::runtime_error("HPACK index out of range");
    return dynamic_[index];
}

size_t Hpack::exact_index(const Header& header) const {
    for (size_t i = 0; i < STATIC_TABLE.size(); ++i)
        if (STATIC_TABLE[i].first == header.name && STATIC_TABLE[i].second == header.value)
            return i + 1;
    for (size_t i = 0; i < dynamic_.size(); ++i)
        if (dynamic_[i].name == header.name && dynamic_[i].value == header.value)
            return STATIC_TABLE.size() + i + 1;
    return 0;
}

size_t Hpack::name_index(std::string_view name) const {
    for (size_t i = 0; i < STATIC_TABLE.size(); ++i)
        if (STATIC_TABLE[i].first == name) return i + 1;
    for (size_t i = 0; i < dynamic_.size(); ++i)
        if (dynamic_[i].name == name) return STATIC_TABLE.size() + i + 1;
    return 0;
}

void Hpack::evict() {
    // 表容量是字节额度，不是最多保存多少项；最旧项最先被赶出“字典”。
    while (table_bytes_ > max_table_bytes_) {
        table_bytes_ -= entry_size(dynamic_.back());
        dynamic_.pop_back();
    }
}

void Hpack::insert(Header header) {
    const size_t bytes = entry_size(header);
    if (bytes > max_table_bytes_) {
        // 单项本身比整个表还大：规范要求清空动态表，但不保存该项。
        dynamic_.clear();
        table_bytes_ = 0;
        return;
    }
    dynamic_.push_front(std::move(header));
    table_bytes_ += bytes;
    evict();
}

void Hpack::set_max_table_size(size_t bytes) {
    if (bytes > 4096) throw std::runtime_error("teaching HPACK table cap is 4096");
    max_table_bytes_ = bytes;
    evict();
}

void Hpack::set_encoder_table_limit(size_t bytes) {
    set_max_table_size(bytes);
    // 本地淘汰后还需通知对端解码器，否则双方索引会指向不同首部。
    emit_table_size_update_ = true;
}

void Hpack::write_integer(std::string& out, size_t value, uint8_t bits, uint8_t mask) {
    // 首字节低 bits 位放整数，高位由 mask 表示“索引/字面量”等表示形式。
    const size_t limit = (1u << bits) - 1;
    if (value < limit) { out.push_back(char(mask | value)); return; }
    out.push_back(char(mask | limit));
    value -= limit;
    // 放不下的部分用 7 位一组续写；每个续字节最高位 1 表示后面还有。
    while (value >= 128) { out.push_back(char((value & 127) | 128)); value >>= 7; }
    out.push_back(char(value));
}

size_t Hpack::read_integer(std::string_view block, size_t& offset, uint8_t bits) {
    if (offset >= block.size()) throw std::runtime_error("truncated HPACK integer");
    const size_t limit = (1u << bits) - 1;
    size_t result = uint8_t(block[offset++]) & limit;
    if (result < limit) return result;
    // 首字节低位全是 1 时，后续字节才是扩展整数；检查截断和溢出。
    unsigned shift = 0;
    for (;;) {
        if (offset >= block.size() || shift >= std::numeric_limits<size_t>::digits)
            throw std::runtime_error("bad HPACK integer");
        const uint8_t byte = uint8_t(block[offset++]);
        if (size_t(byte & 127) > (std::numeric_limits<size_t>::max() - result) >> shift)
            throw std::runtime_error("HPACK integer overflow");
        result += size_t(byte & 127) << shift;
        if (!(byte & 128)) return result;
        shift += 7;
    }
}

void Hpack::write_string(std::string& out, std::string_view value) {
    // 字符串长度首位为 0 表示原始字节；1 才表示 Huffman，本实验不做后者。
    write_integer(out, value.size(), 7, 0);
    out.append(value);
}

std::string Hpack::read_string(std::string_view block, size_t& offset) {
    if (offset >= block.size()) throw std::runtime_error("truncated HPACK string");
    if (uint8_t(block[offset]) & 0x80) throw std::runtime_error("Huffman not taught here");
    const size_t length = read_integer(block, offset, 7);
    if (length > block.size() - offset) throw std::runtime_error("truncated HPACK string");
    std::string value(block.substr(offset, length));
    offset += length;
    return value;
}

std::string Hpack::encode(const std::vector<Header>& headers) {
    std::string out;
    if (emit_table_size_update_) {
        // 0x20 = 001xxxxx：动态表大小更新只能出现在首部块最前面。
        write_integer(out, max_table_bytes_, 5, 0x20);
        emit_table_size_update_ = false;
    }
    for (const Header& header : headers) {
        const size_t exact = exact_index(header);
        // 完全命中时只发一个索引，如静态表 2（:method GET）编码为 0x82。
        if (exact) { write_integer(out, exact, 7, 0x80); continue; }
        // 没有完全命中就发“带索引的字面量”（01xxxxxx）；名称可复用已有索引，
        // 值仍需发原始字符串。双方随后把这一对字段插入动态表。
        const size_t name = name_index(header.name);
        write_integer(out, name, 6, 0x40);
        if (!name) write_string(out, header.name);
        write_string(out, header.value);
        insert(header);
    }
    return out;
}

std::vector<Header> Hpack::decode(std::string_view block) {
    std::vector<Header> headers;
    size_t offset = 0;
    while (offset < block.size()) {
        const uint8_t first = uint8_t(block[offset]);
        if (first & 0x80) {
            // 1xxxxxxx：整个 name/value 均从静态或动态表取出。
            headers.push_back(at(read_integer(block, offset, 7)));
        } else if (first & 0x40) {
            // 01xxxxxx：字面量 + 增量索引，解码后同步插入本方向动态表。
            const size_t index = read_integer(block, offset, 6);
            Header h;
            h.name = index ? at(index).name : read_string(block, offset);
            h.value = read_string(block, offset);
            headers.push_back(h);
            insert(std::move(h));
        } else if (first & 0x20) {
            // 001xxxxx：只改表容量，不产生一条业务首部。
            if (!headers.empty()) throw std::runtime_error("table update must precede headers");
            set_max_table_size(read_integer(block, offset, 5));
        } else {
            // 0000xxxx/0001xxxx：普通或敏感字段的“不索引”字面量；
            // 两者在本教学版都只解码，不改变动态表。
            const size_t index = read_integer(block, offset, 4);
            Header h;
            h.name = index ? at(index).name : read_string(block, offset);
            h.value = read_string(block, offset);
            headers.push_back(std::move(h));
        }
        if (headers.size() > 64) throw std::runtime_error("too many teaching headers");
    }
    return headers;
}

} // namespace learn_h2
