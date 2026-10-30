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
class IFileRepository
{
public:
    virtual ~IFileRepository() = default;
    virtual FileObject createFile(FileObject file) = 0;
    [[nodiscard]] virtual std::optional<FileObject> findFile(FileId id) const = 0;
    virtual bool updateFileProgress(FileId id, UserId owner,
                                    std::uint64_t expectedOffset,
                                    std::uint64_t uploadedSize) = 0;
    virtual bool completeFile(FileId id, UserId owner, std::string sha256,
                              TimePoint now) = 0;
    virtual bool failFile(FileId id, UserId owner) = 0;
    virtual bool attachFile(MessageId message, FileId file) = 0;
    [[nodiscard]] virtual std::vector<MessageAttachment> attachmentsForMessage(
        MessageId message) const = 0;
    [[nodiscard]] virtual bool canAccessFile(UserId user, FileId file) const = 0;
};
}
