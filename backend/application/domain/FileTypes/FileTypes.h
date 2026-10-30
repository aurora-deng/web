#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** FileTypes: protocol/database independent business facts. */
enum class FileState
{
    Pending,
    Ready,
    Failed,
    Deleted
};

struct FileObject
{
    FileId id = 0;
    UserId ownerId = 0;
    std::string originalName;
    std::string mediaType;
    std::uint64_t expectedSize = 0;
    std::uint64_t uploadedSize = 0;
    std::string sha256;
    FileState state = FileState::Pending;
    TimePoint createdAt{};
    std::optional<TimePoint> readyAt;
};

struct MessageAttachment
{
    FileId fileId = 0;
    std::string originalName;
    std::string mediaType;
    std::uint64_t size = 0;
    std::string sha256;
};

struct StoredFileSummary
{
    std::uint64_t size = 0;
    std::string sha256;
    std::string prefix;
};

struct FileDownload
{
    FileObject file;
    std::string storagePath;
};
}
