#pragma once

#include "server/phase11/ports/Ports/Ports.h"

#include <filesystem>
#include <mutex>

namespace webserver::phase11
{

/**
 * 本地不可变文件仓库。临时分片写入 pending，封存后原子 rename 到 objects；路径只由
 * 数字 FileId 生成，永远不把用户文件名拼进磁盘路径，从根源上避开目录穿越。
 */
class LocalFileStorage final : public IFileStorage
{
public:
    explicit LocalFileStorage(std::filesystem::path root);

    void prepare(FileId file) override;
    std::uint64_t pendingSize(FileId file) const override;
    std::uint64_t append(FileId file, std::uint64_t offset,
                         std::string_view bytes) override;
    StoredFileSummary finalize(FileId file) override;
    std::string readyPath(FileId file) const override;
    void remove(FileId file) noexcept override;

private:
    std::filesystem::path pendingPath(FileId file) const;
    std::filesystem::path objectPath(FileId file) const;
    static StoredFileSummary inspect(const std::filesystem::path &path);

    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

} // namespace webserver::phase11
