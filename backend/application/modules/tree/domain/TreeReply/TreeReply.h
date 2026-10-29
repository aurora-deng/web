#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
namespace webserver::function3 {
struct TreeReply {
    Id id{};
    Id post{};
    Id author{};
    std::string authorName;
    std::string requestId;
    std::string body;
    Milliseconds createdAt{};
    bool deleted{};
    Id parent{}; // 0 为主评论；关系持久化在 tree_reply_links，不借助正文伪造楼中楼。
    std::string replyToName;
};
}
