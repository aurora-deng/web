#pragma once
#include "backend/application/modules/contacts/domain/ContactDirectoryTypes/ContactDirectoryTypes.h"
#include "backend/application/domain/Domain/Domain.h"
namespace webserver::chat {
    using namespace webserver::phase11;
    using contacts::UserCard;
    using contacts::UserPage;
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
