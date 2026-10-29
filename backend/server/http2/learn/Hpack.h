#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace learn_h2 {

struct Header { std::string name; std::string value; };

// 把 HPACK 想成“每个通信方向各一本首部字典”：客户端编码器与服务端解码器
// 必须同步增删条目，不能给每个 stream 单独建表，也不能让两个方向共用一张表。
class Hpack {
public:
    // 把 name/value 变成 HEADERS 帧的二进制 payload；decode 做反向工作。
    std::string encode(const std::vector<Header>& headers);
    std::vector<Header> decode(std::string_view block);
    // 解码收到的 HPACK 表大小更新；编码端在对端 SETTINGS 限制变化后调用下者。
    void set_max_table_size(size_t bytes);
    void set_encoder_table_limit(size_t bytes);
    size_t dynamic_entries() const { return dynamic_.size(); }

private:
    // HPACK 索引从 1 开始：1..61 是静态表，62 起是最新插入的动态条目。
    const Header& at(size_t one_based_index) const;
    size_t exact_index(const Header& header) const;
    size_t name_index(std::string_view name) const;
    void insert(Header header);
    void evict();
    static void write_integer(std::string& out, size_t value, uint8_t prefix_bits,
                              uint8_t prefix_mask);
    static size_t read_integer(std::string_view block, size_t& offset, uint8_t prefix_bits);
    static void write_string(std::string& out, std::string_view value);
    static std::string read_string(std::string_view block, size_t& offset);

    // push_front 让最新条目位于 62；超出容量时从队尾淘汰最旧条目。
    std::deque<Header> dynamic_;
    size_t table_bytes_ = 0;
    size_t max_table_bytes_ = 4096;
    // SETTINGS 改变编码表上限后，下一个首部块开头必须先声明新大小。
    bool emit_table_size_update_ = false;
};

} // namespace learn_h2
