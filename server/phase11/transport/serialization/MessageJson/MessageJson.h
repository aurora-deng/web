#pragma once

#include "server/phase11/domain/Domain/Domain.h"

#include <string>
#include <string_view>

namespace webserver::phase11::transport
{

/** 同一条持久化消息在 HTTP 历史和 WebSocket 广播中使用同一字段格式。 */
std::string serializeMessageJson(const Message &message,
                                 std::string_view eventType = {});

} // namespace webserver::phase11::transport
