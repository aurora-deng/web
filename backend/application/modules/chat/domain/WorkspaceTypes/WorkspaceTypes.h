#pragma once
#include "backend/application/domain/Domain/Domain.h"
namespace webserver::chat {
    using namespace webserver::phase11;
    struct UserCard {
        UserId id{
        };
        std::string username,displayName;
        std::optional<FileId>avatar;
        bool ai{
        }
        ,friendship{
        }
        ,pending{
        };
    };
    struct UserPage {
        std::vector<UserCard>items;
        UserId nextCursor{
        };
    };
    struct MessagePage {
        std::vector<Message>items;
        MessageId nextCursor{
        };
    };
    struct ConversationPreference {
        ConversationId conversation{
        };
        UserId user{
        };
        bool muted{
        };
        std::uint64_t version{
        };
    };
    struct PreferenceWrite {
        ConversationPreference value;
        bool changed{
        };
    };
    struct MessageContext {
        Message target;
        std::vector<Message>items;
    };
}
