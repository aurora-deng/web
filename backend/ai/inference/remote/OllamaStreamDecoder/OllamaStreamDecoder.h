#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11
{

/** Ollama /api/chat 的一行 NDJSON 所表达的增量事件。 */
struct OllamaStreamEvent
{
    std::string content;
    std::string error;
    bool done = false;
};

/**
 * 增量解析 Ollama 的 NDJSON 响应。
 *
 * 网络层可能把一行拆成多个回调，也可能一次交付多行；Decoder 像“流水线切刀”，
 * 先按换行恢复完整 JSON，再只提取 message.content、done 和 error。它不是通用 JSON
 * DOM，避免为三个字段复制整棵对象，同时仍严格处理转义、Unicode、嵌套与畸形输入。
 */
class OllamaStreamDecoder final
{
public:
    explicit OllamaStreamDecoder(std::size_t maxLineBytes = 1024 * 1024);

    [[nodiscard]] std::vector<OllamaStreamEvent> feed(std::string_view bytes);
    [[nodiscard]] std::vector<OllamaStreamEvent> finish();
    [[nodiscard]] bool sawTerminalEvent() const noexcept { return terminal_; }

private:
    std::vector<OllamaStreamEvent> consumeCompleteLines(bool flushTail);

    std::size_t maxLineBytes_;
    std::string pending_;
    bool terminal_ = false;
};

} // namespace webserver::phase11
