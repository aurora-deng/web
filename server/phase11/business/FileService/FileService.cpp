#include "server/phase11/business/FileService/FileService.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace webserver::phase11
{
namespace
{
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string extensionOf(std::string_view name)
{
    return lower(std::filesystem::path(std::string(name)).extension().string());
}

bool begins(std::string_view value, std::initializer_list<unsigned char> bytes)
{
    if (value.size() < bytes.size()) return false;
    std::size_t index = 0;
    for (const auto byte : bytes)
        if (static_cast<unsigned char>(value[index++]) != byte) return false;
    return true;
}

bool zipSignature(std::string_view value)
{
    return begins(value, {'P', 'K', 3, 4}) || begins(value, {'P', 'K', 5, 6}) ||
           begins(value, {'P', 'K', 7, 8});
}
} // namespace

std::string FileService::normalizeName(std::string name)
{
    if (name.empty() || name.size() > 255 || name == "." || name == "..")
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "file name length is invalid");
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "file name must not contain a path");
    for (const unsigned char c : name)
        if (c < 0x20 || c == 0x7f)
            throw ApplicationError(ErrorCode::InvalidArgument,
                                   "file name contains a control character");
    return name;
}

std::string FileService::normalizeMediaType(std::string mediaType)
{
    const auto semicolon = mediaType.find(';');
    if (semicolon != std::string::npos) mediaType.erase(semicolon);
    while (!mediaType.empty() && std::isspace(
               static_cast<unsigned char>(mediaType.back())))
        mediaType.pop_back();
    std::size_t begin = 0;
    while (begin < mediaType.size() && std::isspace(
               static_cast<unsigned char>(mediaType[begin]))) ++begin;
    mediaType.erase(0, begin);
    mediaType = lower(std::move(mediaType));
    if (mediaType.empty() || mediaType.size() > 127)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "media type is invalid");
    return mediaType;
}

void FileService::validateType(std::string_view name,
                               std::string_view mediaType,
                               std::uint64_t size)
{
    if (size == 0 || size > kMaximumFileBytes)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "file size must be between 1 byte and 25 MiB");
    const auto extension = extensionOf(name);
    static const std::map<std::string, std::vector<std::string>> allowed{
        {".png", {"image/png"}},
        {".jpg", {"image/jpeg"}}, {".jpeg", {"image/jpeg"}},
        {".gif", {"image/gif"}}, {".webp", {"image/webp"}},
        {".pdf", {"application/pdf"}},
        {".txt", {"text/plain"}}, {".md", {"text/plain", "text/markdown"}},
        {".json", {"application/json", "text/json"}},
        {".zip", {"application/zip", "application/x-zip-compressed"}},
        {".docx", {"application/vnd.openxmlformats-officedocument.wordprocessingml.document"}},
        {".xlsx", {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}},
        {".pptx", {"application/vnd.openxmlformats-officedocument.presentationml.presentation"}},
        {".bin", {"application/octet-stream"}}
    };
    const auto found = allowed.find(extension);
    if (found == allowed.end() ||
        std::find(found->second.begin(), found->second.end(), mediaType) ==
            found->second.end())
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "file extension and media type are not allowed");
    if (mediaType.starts_with("image/") && size > kMaximumImageBytes)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "image size exceeds 10 MiB");
}

void FileService::validateSignature(const FileObject &file,
                                    const StoredFileSummary &stored)
{
    const auto &value = stored.prefix;
    // 即使扩展名伪装成 .bin，也拒绝常见的 Windows PE 与 ELF 可执行文件。
    if (begins(value, {'M', 'Z'}) || begins(value, {0x7f, 'E', 'L', 'F'}))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "executable file content is not allowed");
    bool valid = true;
    if (file.mediaType == "image/png")
        valid = begins(value, {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a});
    else if (file.mediaType == "image/jpeg")
        valid = begins(value, {0xff, 0xd8, 0xff});
    else if (file.mediaType == "image/gif")
        valid = value.starts_with("GIF87a") || value.starts_with("GIF89a");
    else if (file.mediaType == "image/webp")
        valid = value.size() >= 12 && value.substr(0, 4) == "RIFF" &&
                value.substr(8, 4) == "WEBP";
    else if (file.mediaType == "application/pdf")
        valid = value.starts_with("%PDF-");
    else if (file.mediaType == "application/zip" ||
             file.mediaType == "application/x-zip-compressed" ||
             extensionOf(file.originalName) == ".docx" ||
             extensionOf(file.originalName) == ".xlsx" ||
             extensionOf(file.originalName) == ".pptx")
        valid = zipSignature(value);
    if (!valid)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "file content does not match the declared type");
}

FileObject FileService::createUpload(UserId owner, std::string originalName,
                                     std::string mediaType,
                                     std::uint64_t expectedSize,
                                     TimePoint now)
{
    if (!users_.findUser(owner))
        throw ApplicationError(ErrorCode::NotFound, "file owner not found");
    originalName = normalizeName(std::move(originalName));
    mediaType = normalizeMediaType(std::move(mediaType));
    validateType(originalName, mediaType, expectedSize);

    auto transaction = database_.beginTransaction();
    auto file = files_.createFile({0, owner, std::move(originalName),
                                   std::move(mediaType), expectedSize, 0, {},
                                   FileState::Pending, now, {}});
    try
    {
        storage_.prepare(file.id);
    }
    catch (...)
    {
        (void)files_.failFile(file.id, owner);
        throw ApplicationError(ErrorCode::Unavailable,
                               "file storage is unavailable");
    }
    transaction->commit();
    return file;
}

FileObject FileService::appendChunk(UserId owner, FileId fileId,
                                    std::uint64_t offset,
                                    std::string_view bytes)
{
    if (bytes.empty() || bytes.size() > kMaximumChunkBytes)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "chunk size must be between 1 byte and 256 KiB");
    auto file = files_.findFile(fileId);
    if (!file) throw ApplicationError(ErrorCode::NotFound, "file not found");
    if (file->ownerId != owner)
        throw ApplicationError(ErrorCode::Forbidden, "file owner mismatch");
    if (file->state != FileState::Pending)
        throw ApplicationError(ErrorCode::Conflict, "upload is not pending");
    if (offset != file->uploadedSize ||
        bytes.size() > file->expectedSize - file->uploadedSize)
        throw ApplicationError(ErrorCode::Conflict, "upload offset mismatch");

    std::uint64_t storedSize = 0;
    try
    {
        storedSize = storage_.append(fileId, offset, bytes);
    }
    catch (const std::invalid_argument &)
    {
        throw ApplicationError(ErrorCode::Conflict, "upload offset mismatch");
    }
    catch (...)
    {
        throw ApplicationError(ErrorCode::Unavailable,
                               "file storage is unavailable");
    }
    auto transaction = database_.beginTransaction();
    if (!files_.updateFileProgress(fileId, owner, offset, storedSize))
        throw ApplicationError(ErrorCode::Conflict,
                               "upload progress changed concurrently");
    transaction->commit();
    file = files_.findFile(fileId);
    if (!file) throw ApplicationError(ErrorCode::Unavailable,
                                      "upload metadata disappeared");
    return *file;
}

FileObject FileService::completeUpload(UserId owner, FileId fileId,
                                       TimePoint now)
{
    auto file = files_.findFile(fileId);
    if (!file) throw ApplicationError(ErrorCode::NotFound, "file not found");
    if (file->ownerId != owner)
        throw ApplicationError(ErrorCode::Forbidden, "file owner mismatch");
    if (file->state == FileState::Ready) return *file;
    if (file->state != FileState::Pending ||
        file->uploadedSize != file->expectedSize)
        throw ApplicationError(ErrorCode::Conflict, "upload is incomplete");

    StoredFileSummary stored;
    try { stored = storage_.finalize(fileId); }
    catch (...)
    {
        throw ApplicationError(ErrorCode::Unavailable,
                               "file storage could not finalize the upload");
    }
    if (stored.size != file->expectedSize)
        throw ApplicationError(ErrorCode::Conflict,
                               "stored file size does not match upload metadata");
    try { validateSignature(*file, stored); }
    catch (...)
    {
        storage_.remove(fileId);
        auto transaction = database_.beginTransaction();
        (void)files_.failFile(fileId, owner);
        transaction->commit();
        throw;
    }
    auto transaction = database_.beginTransaction();
    if (!files_.completeFile(fileId, owner, stored.sha256, now))
    {
        // 两个“完成”请求可能同时通过上面的 pending 检查。数据库更新是最终裁判：
        // 输掉竞争的一方重新读取，若赢家已经用同一不可变文件完成，就按幂等成功返回。
        const auto completed = files_.findFile(fileId);
        if (completed && completed->ownerId == owner &&
            completed->state == FileState::Ready &&
            completed->sha256 == stored.sha256)
        {
            transaction->commit();
            return *completed;
        }
        throw ApplicationError(ErrorCode::Conflict,
                               "upload metadata could not be finalized");
    }
    transaction->commit();
    file = files_.findFile(fileId);
    if (!file) throw ApplicationError(ErrorCode::Unavailable,
                                      "upload metadata disappeared");
    return *file;
}

FileDownload FileService::download(UserId requester, FileId fileId) const
{
    const auto file = files_.findFile(fileId);
    if (!file || file->state != FileState::Ready)
        throw ApplicationError(ErrorCode::NotFound, "file not found");
    if (!files_.canAccessFile(requester, fileId))
        throw ApplicationError(ErrorCode::Forbidden, "file access denied");
    return {*file, storage_.readyPath(fileId)};
}

User FileService::useAsAvatar(UserId owner, FileId fileId)
{
    const auto file = files_.findFile(fileId);
    if (!file || file->state != FileState::Ready)
        throw ApplicationError(ErrorCode::Conflict, "avatar upload is not ready");
    if (file->ownerId != owner)
        throw ApplicationError(ErrorCode::Forbidden, "file owner mismatch");
    if (!file->mediaType.starts_with("image/"))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "avatar must be an image");
    auto transaction = database_.beginTransaction();
    if (!users_.updateAvatar(owner, fileId))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    transaction->commit();
    const auto user = users_.findUser(owner);
    if (!user) throw ApplicationError(ErrorCode::Unavailable,
                                      "updated user could not be loaded");
    return *user;
}

} // namespace webserver::phase11
