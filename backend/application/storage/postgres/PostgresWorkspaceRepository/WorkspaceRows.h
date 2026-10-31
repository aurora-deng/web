#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace webserver::phase11::workspace_rows {
/** Internal DAO projection of users + relationship joins. Never returned by a
 * business port. Password/session columns are deliberately absent. A join row
 * is not falsely presented as a complete one-table users persistence object. */
struct DirectoryRow {
    std::uint64_t id;
    std::string username,displayName;
    std::optional<std::uint64_t> avatar;
    bool ai,friendship,pending;
};
}
