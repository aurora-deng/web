#include "server/phase11/storage/files/LocalFileStorage/LocalFileStorage.h"

#include <openssl/evp.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace webserver::phase11
{
namespace
{
struct FileDescriptor
{
    int value = -1;
    ~FileDescriptor() { if (value >= 0) ::close(value); }
};

std::runtime_error ioError(std::string_view operation)
{
    return std::runtime_error(std::string(operation) + ": " +
                              std::strerror(errno));
}

std::string hex(const unsigned char *bytes, std::size_t size)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index)
        stream << std::setw(2) << static_cast<unsigned>(bytes[index]);
    return stream.str();
}
} // namespace

LocalFileStorage::LocalFileStorage(std::filesystem::path root)
{
    if (root.empty()) throw std::invalid_argument("file storage root is empty");
    std::filesystem::create_directories(root / "pending");
    std::filesystem::create_directories(root / "objects");
    root_ = std::filesystem::canonical(root);
    ::chmod(root_.c_str(), 0700);
    ::chmod((root_ / "pending").c_str(), 0700);
    ::chmod((root_ / "objects").c_str(), 0700);
}

std::filesystem::path LocalFileStorage::pendingPath(FileId file) const
{
    if (!file) throw std::invalid_argument("invalid file id");
    return root_ / "pending" / (std::to_string(file) + ".part");
}

std::filesystem::path LocalFileStorage::objectPath(FileId file) const
{
    if (!file) throw std::invalid_argument("invalid file id");
    return root_ / "objects" / std::to_string(file / 1000) /
           std::to_string(file);
}

void LocalFileStorage::prepare(FileId file)
{
    std::lock_guard lock(mutex_);
    const auto path = pendingPath(file);
    FileDescriptor fd{::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY
#ifdef O_NOFOLLOW
                                      | O_NOFOLLOW
#endif
                                      , 0600)};
    if (fd.value < 0) throw ioError("create pending file");
}

std::uint64_t LocalFileStorage::pendingSize(FileId file) const
{
    std::lock_guard lock(mutex_);
    struct stat status{};
    if (::lstat(pendingPath(file).c_str(), &status) != 0)
        throw ioError("stat pending file");
    if (!S_ISREG(status.st_mode)) throw std::runtime_error("pending path is not a file");
    return static_cast<std::uint64_t>(status.st_size);
}

std::uint64_t LocalFileStorage::append(FileId file, std::uint64_t offset,
                                       std::string_view bytes)
{
    std::lock_guard lock(mutex_);
    FileDescriptor fd{::open(pendingPath(file).c_str(), O_RDWR
#ifdef O_NOFOLLOW
                                      | O_NOFOLLOW
#endif
                                      )};
    if (fd.value < 0) throw ioError("open pending file");
    struct stat status{};
    if (::fstat(fd.value, &status) != 0) throw ioError("stat pending file");
    if (!S_ISREG(status.st_mode)) throw std::runtime_error("pending path is not a file");
    const auto actual = static_cast<std::uint64_t>(status.st_size);

    // DB 在落盘后短暂失败时，客户端会重试相同 offset。若磁盘已经拥有完全相同的
    // 那一块，就把它当成幂等重放，只推进数据库游标，不重复追加。
    if (actual == offset + bytes.size())
    {
        std::string existing(bytes.size(), '\0');
        const auto read = ::pread(fd.value, existing.data(), existing.size(),
                                  static_cast<off_t>(offset));
        if (read == static_cast<ssize_t>(existing.size()) && existing == bytes)
            return actual;
    }
    if (actual != offset) throw std::invalid_argument("file offset mismatch");
    std::size_t written = 0;
    while (written < bytes.size())
    {
        const auto count = ::pwrite(fd.value, bytes.data() + written,
                                    bytes.size() - written,
                                    static_cast<off_t>(offset + written));
        if (count < 0)
        {
            if (errno == EINTR) continue;
            throw ioError("write pending file");
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fdatasync(fd.value) != 0) throw ioError("sync pending file");
    return offset + written;
}

StoredFileSummary LocalFileStorage::inspect(const std::filesystem::path &path)
{
    FileDescriptor fd{::open(path.c_str(), O_RDONLY
#ifdef O_NOFOLLOW
                                      | O_NOFOLLOW
#endif
                                      )};
    if (fd.value < 0) throw ioError("open stored file");
    struct stat status{};
    if (::fstat(fd.value, &status) != 0) throw ioError("stat stored file");
    if (!S_ISREG(status.st_mode)) throw std::runtime_error("stored path is not a file");

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 initialization failed");
    std::array<char, 64 * 1024> buffer{};
    std::string prefix;
    for (;;)
    {
        const auto count = ::read(fd.value, buffer.data(), buffer.size());
        if (count < 0)
        {
            if (errno == EINTR) continue;
            throw ioError("read stored file");
        }
        if (count == 0) break;
        if (prefix.size() < 32)
            prefix.append(buffer.data(), std::min<std::size_t>(
                static_cast<std::size_t>(count), 32 - prefix.size()));
        if (EVP_DigestUpdate(digest.get(), buffer.data(),
                             static_cast<std::size_t>(count)) != 1)
            throw std::runtime_error("SHA-256 update failed");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int outputSize = 0;
    if (EVP_DigestFinal_ex(digest.get(), output.data(), &outputSize) != 1)
        throw std::runtime_error("SHA-256 finalization failed");
    return {static_cast<std::uint64_t>(status.st_size),
            hex(output.data(), outputSize), std::move(prefix)};
}

StoredFileSummary LocalFileStorage::finalize(FileId file)
{
    std::lock_guard lock(mutex_);
    const auto pending = pendingPath(file);
    const auto object = objectPath(file);
    if (std::filesystem::exists(object)) return inspect(object);
    const auto summary = inspect(pending);
    std::filesystem::create_directories(object.parent_path());
    ::chmod(object.parent_path().c_str(), 0700);
    if (::rename(pending.c_str(), object.c_str()) != 0)
        throw ioError("finalize stored file");
    ::chmod(object.c_str(), 0600);
    return summary;
}

std::string LocalFileStorage::readyPath(FileId file) const
{
    std::lock_guard lock(mutex_);
    const auto path = objectPath(file);
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
        throw std::runtime_error("stored file is missing");
    return path.string();
}

void LocalFileStorage::remove(FileId file) noexcept
{
    try
    {
        std::lock_guard lock(mutex_);
        std::error_code ignored;
        std::filesystem::remove(pendingPath(file), ignored);
        std::filesystem::remove(objectPath(file), ignored);
    }
    catch (...) {}
}

} // namespace webserver::phase11
