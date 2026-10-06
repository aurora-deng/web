#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/ports/Ports/Ports.h"

namespace webserver::phase11
{

/** 文件上传、内容核验、下载授权和头像绑定的业务边界。 */
class FileService final : public IFileBusiness
{
public:
    static constexpr std::uint64_t kMaximumFileBytes = 25ULL * 1024 * 1024;
    static constexpr std::uint64_t kMaximumImageBytes = 10ULL * 1024 * 1024;
    static constexpr std::size_t kMaximumChunkBytes = 256 * 1024;

    FileService(IDatabase &database, IUserRepository &users,
                IFileRepository &files, IFileStorage &storage)
        : database_(database), users_(users), files_(files), storage_(storage)
    {
    }

    FileObject createUpload(UserId owner, std::string originalName,
                            std::string mediaType,
                            std::uint64_t expectedSize,
                            TimePoint now) override;
    FileObject appendChunk(UserId owner, FileId file,
                           std::uint64_t offset,
                           std::string_view bytes) override;
    FileObject completeUpload(UserId owner, FileId file,
                              TimePoint now) override;
    FileDownload download(UserId requester, FileId file) const override;
    User useAsAvatar(UserId owner, FileId file) override;

private:
    static std::string normalizeName(std::string name);
    static std::string normalizeMediaType(std::string mediaType);
    static void validateType(std::string_view name, std::string_view mediaType,
                             std::uint64_t size);
    static void validateSignature(const FileObject &file,
                                  const StoredFileSummary &stored);

    IDatabase &database_;
    IUserRepository &users_;
    IFileRepository &files_;
    IFileStorage &storage_;
};

} // namespace webserver::phase11
