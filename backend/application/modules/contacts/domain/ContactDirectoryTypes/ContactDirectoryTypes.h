#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include <optional>
#include <cstddef>
#include <string>
#include <vector>
namespace webserver::contacts {
/** Public read projections; credentials and private profile fields never enter these views. */
struct UserCard {
    phase11::UserId id{};
    std::string username,displayName;
    std::optional<phase11::FileId>avatar;
    bool ai{},friendship{},pending{};
};
struct UserPage {std::vector<UserCard>items;phase11::UserId nextCursor{};};
struct UserDirectoryQuery {std::string text;phase11::UserId after=0;std::size_t limit=20;};
}
