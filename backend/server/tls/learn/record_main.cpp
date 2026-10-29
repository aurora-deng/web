#include "Record.h"

#include <cassert>
#include <iostream>

int main() {
    // 两个合成记录：type=22 握手，type=23 应用数据。只演示信封分帧，
    // payload 在此不是合法 TLS 消息，更不是手写的加密算法。
    // 记录头含 NUL；显式写长度，避免 const char* 构造在第一个 NUL 截断。
    const std::string wire = std::string("\x16\x03\x03\x00\x03" "abc", 8) +
                             std::string("\x17\x03\x03\x00\x02" "xy", 7);
    RecordParser parser;
    std::vector<Record> records;
    for (char byte : wire) {
        const auto part = parser.feed(std::string_view(&byte, 1));
        records.insert(records.end(), part.begin(), part.end());
    }
    assert(records.size() == 2);
    assert(records[0].outerType == 22 && records[0].payload == "abc");
    assert(records[1].outerType == 23 && records[1].payload == "xy");
    std::cout << "TLS outer-record splitting PASS\n";
}
