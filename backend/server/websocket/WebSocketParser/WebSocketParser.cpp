// ==============================================================================
// 文件名：WebSocketParser.cpp
// 职责比喻：这是"WebSocket 帧分拣总调度"的实际工作流程（对称 HttpParser.cpp）。
//   实现 WebSocketParser.h 中声明的 parse() 主循环和 reset()。parse() 是状态机的"心脏"：
//   它在一个 while(true) 里根据当前 stage 调用对应子解析器，根据返回结果决定切换到哪个
//   状态、还是返回（等数据 / 出错 / 完成）。整个 WebSocket 帧的"拆包"过程就在这里被串起来。
//
// 关键技术点（初学者重点理解）：
//   1. 【状态机循环】while(true) + switch(stage) 是状态机的经典写法。每个 case 处理一个阶段，
//      子解析器返回非 Ok 就立即 return（暂停状态机），下次 parse() 从同一 stage 继续。
//   2. 【结果只交付一次】COMPLETE 状态用 resultDelivered_ 保证一个完整帧只返回一次 Ok，
//      防止误用导致"一个帧被处理两次"。
//   3. 【Close 帧特殊处理】解析到 opcode==Close 时返回 Closed 而非 Ok，通知上层连接要关闭。
//   4. 【reset 不清 Buffer】连接复用时，Buffer 里可能已有下一帧的数据，reset 只重置解析器状态，
//      保留 Buffer 内容，让下一轮直接解析。
// ==============================================================================
#include "WebSocketParser.h"

/**
 * @brief 解析主循环：状态机持续推进
 * @param buffer 连接读缓冲区
 * @param out 输出帧对象
 * @return WsDecodeResult::Ok（完整）/ NeedMore（等数据）/ Error（出错）/ Closed（Close 帧）
 *
 * 通俗解释：调度员站在传送带前，看当前该干哪步（stage）：
 *   - BASE_HEADER：让基础头拆解员拆 2 字节头；拆完进入 EXT_LENGTH；
 *   - EXT_LENGTH：让扩展长度展开员读长度纸条；读完进入 MASK_KEY；
 *   - MASK_KEY：让掩码密钥收取员收 4 字节钥匙；收完进入 PAYLOAD；
 *   - PAYLOAD：让货物取出员取 payload 并解掩码；取完进入 COMPLETE；
 *   - COMPLETE：交付一次 Ok（Close 帧交付 Closed），第二次再进这里就报错（防重复交付）。
 *   任何子拆解员说"数据不够"或"出错"，调度员立刻 return，等下次新数据再来继续。
 */
WsDecodeResult WebSocketParser::parse(Buffer &buffer, WsFrame &out)
{
    while (true)
    {
        switch (stage_)
        {
        // ---- 阶段 1：解析 2 字节基础帧头（FIN/RSV/opcode/MASK/len7） ----
        case WsParseStage::BASE_HEADER:
        {
            auto r = frameHeaderParser_.parse(buffer, ctx_);
            if (r != WsDecodeResult::Ok)
            {
                if (r == WsDecodeResult::Error)
                    stage_ = WsParseStage::ERROR;
                return r;
            }
            stage_ = WsParseStage::EXT_LENGTH;
            break;
        }
        // ---- 阶段 2：展开 126/127 扩展长度（len7 < 126 时直接通过） ----
        case WsParseStage::EXT_LENGTH:
        {
            auto r = extendedLengthParser_.parse(buffer, ctx_);
            if (r != WsDecodeResult::Ok)
            {
                if (r == WsDecodeResult::Error)
                    stage_ = WsParseStage::ERROR;
                return r;
            }
            stage_ = WsParseStage::MASK_KEY;
            break;
        }
        // ---- 阶段 3：读取 4 字节掩码密钥 ----
        case WsParseStage::MASK_KEY:
        {
            auto r = maskKeyParser_.parse(buffer, ctx_);
            if (r != WsDecodeResult::Ok)
            {
                if (r == WsDecodeResult::Error)
                    stage_ = WsParseStage::ERROR;
                return r;
            }
            stage_ = WsParseStage::PAYLOAD;
            break;
        }
        // ---- 阶段 4：读取 payload 并解掩码 ----
        case WsParseStage::PAYLOAD:
        {
            auto r = payloadParser_.parse(buffer, ctx_, out);
            if (r != WsDecodeResult::Ok)
                return r;
            stage_ = WsParseStage::COMPLETE;
            break;
        }
        // ---- 阶段 5：完成态，交付一次 Ok（Close 帧交付 Closed） ----
        case WsParseStage::COMPLETE:
            // 一个完整帧只能交付一次；调用方必须 reset 后才能解析 Buffer 中的下一帧。
            // 这可避免独立使用 Parser 时重复调用 parse 得到第二个虚假的 Ok。
            if (resultDelivered_)
                return WsDecodeResult::Error;
            resultDelivered_ = true;
            // Close 帧特殊处理：返回 Closed 通知上层关闭连接
            if (out.opcode == WsOpcode::Close)
                return WsDecodeResult::Closed;
            return WsDecodeResult::Ok;
        default:
            return WsDecodeResult::Error;
        }
    }
}

/**
 * @brief 重置解析器，准备解析下一个帧
 *
 * 通俗解释：调度员把状态机拨回 BASE_HEADER 起点，清空流转单和四个子拆解员的进度记录，
 *   准备迎接下一个包裹。但传送带（Buffer）上剩下的货不动——那可能是下一个帧的开头。
 */
void WebSocketParser::reset()
{
    // 只重置解析器自身，不清空 Buffer：其中可能已包含下一帧的数据，
    // 保留未消费字节可让 Session 下一轮直接解析，实现安全的连接复用。
    stage_ = WsParseStage::BASE_HEADER;
    resultDelivered_ = false;
    ctx_ = WsParseContext{};
    frameHeaderParser_.reset();
    extendedLengthParser_.reset();
    maskKeyParser_.reset();
    payloadParser_.reset();
}
