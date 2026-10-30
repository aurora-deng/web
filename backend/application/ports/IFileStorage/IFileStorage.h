#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/FileTypes/FileTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IFileStorage
{
public:
    virtual ~IFileStorage() = default;
    virtual void prepare(FileId file) = 0;
    [[nodiscard]] virtual std::uint64_t pendingSize(FileId file) const = 0;
    virtual std::uint64_t append(FileId file, std::uint64_t offset,
                                 std::string_view bytes) = 0;
    virtual StoredFileSummary finalize(FileId file) = 0;
    [[nodiscard]] virtual std::string readyPath(FileId file) const = 0;
    virtual void remove(FileId file) noexcept = 0;
};
}
