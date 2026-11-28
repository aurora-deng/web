#pragma once
#include "backend/application/modules/files/domain/FileTypes/FileTypes.h"
#include "backend/application/modules/accounts/domain/AccountTypes/AccountTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

class IFileBusiness
{
public:
    virtual ~IFileBusiness() = default;
    virtual FileObject createUpload(UserId owner, std::string originalName,
                                    std::string mediaType,
                                    std::uint64_t expectedSize,
                                    TimePoint now) = 0;
    virtual FileObject appendChunk(UserId owner, FileId file,
                                   std::uint64_t offset,
                                   std::string_view bytes) = 0;
    virtual FileObject completeUpload(UserId owner, FileId file,
                                      TimePoint now) = 0;
    [[nodiscard]] virtual FileDownload download(UserId requester,
                                                FileId file) const = 0;
    virtual User useAsAvatar(UserId owner, FileId file) = 0;
};

} // namespace webserver::phase11
