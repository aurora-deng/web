#pragma once
#include "backend/application/modules/contacts/domain/ContactDirectoryTypes/ContactDirectoryTypes.h"
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include <cstddef>
#include <string>

namespace webserver::chat {
/** Application inputs: trusted actor comes separately from authentication.
 * They carry no HTTP/SQL type, connection, JSON map or database row. */
using contacts::UserDirectoryQuery;
struct MessageSearchQuery {
    phase11::ConversationId conversation = 0;
    std::string text;
    phase11::MessageId before = 0;
    std::size_t limit = 30;
};
struct MessageContextQuery {
    phase11::ConversationId conversation = 0;
    phase11::MessageId message = 0;
    std::size_t radius = 20;
};
struct SetConversationPreference {
    phase11::ConversationId conversation = 0;
    bool muted = false;
};
}
