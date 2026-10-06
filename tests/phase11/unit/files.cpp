#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/business/FileService/FileService.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "server/phase11/storage/files/LocalFileStorage/LocalFileStorage.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace webserver::phase11;

#define CHECK(expression)                                                       \
    do                                                                          \
    {                                                                           \
        if (!(expression))                                                      \
        {                                                                       \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__    \
                      << ':' << __LINE__ << '\n';                               \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

namespace
{

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("phase11-files-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path &path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

template <typename Operation>
void expectError(ErrorCode expected, Operation operation)
{
    try
    {
        operation();
        CHECK(false && "expected ApplicationError");
    }
    catch (const ApplicationError &error)
    {
        CHECK(error.code() == expected);
    }
}

User createUser(InMemoryStore &store, std::string username,
                std::string displayName, TimePoint now)
{
    return store.create({0, std::move(username), std::move(displayName), {},
                         "test-password", false, false, now});
}

void makeFriends(InMemoryStore &store, UserId sender, UserId receiver,
                 TimePoint now)
{
    const auto request = store.createFriendRequest(
        {0, sender, receiver, FriendRequestState::Pending, now, {}});
    CHECK(store.decideFriendRequest(request.id, receiver,
                                    FriendRequestState::Accepted, now));
}

void verifyFileLifecycle()
{
    const auto now = std::chrono::system_clock::now();
    TemporaryDirectory directory;
    InMemoryStore store;
    LocalFileStorage storage(directory.path());
    FileService files(store, store, store, storage);
    ChatService chat(store, store, store, store, store, store, store);

    const auto alice = createUser(store, "alice-files", "Alice", now);
    const auto bob = createUser(store, "bob-files", "Bob", now);
    const auto charlie = createUser(store, "charlie-files", "Charlie", now);
    makeFriends(store, alice.id, bob.id, now);
    const auto direct = chat.createDirectConversation(alice.id, bob.id, now);

    // 这里不需要完整 PNG 解码器：业务层只负责协议边界的魔数核验；真正的图片
    // 解码、缩略图和病毒扫描应由后续独立媒体流水线完成。
    const std::string png =
        std::string("\x89PNG\r\n\x1a\n", 8) + "phase17-image-payload";
    auto upload = files.createUpload(alice.id, "avatar.png", "image/png",
                                     png.size(), now);
    CHECK(upload.state == FileState::Pending && upload.uploadedSize == 0);

    const auto firstSize = std::size_t{11};
    upload = files.appendChunk(alice.id, upload.id, 0,
                               std::string_view(png).substr(0, firstSize));
    CHECK(upload.uploadedSize == firstSize);
    expectError(ErrorCode::Conflict, [&] {
        (void)files.appendChunk(alice.id, upload.id, 0, "wrong retry");
    });
    upload = files.appendChunk(
        alice.id, upload.id, firstSize,
        std::string_view(png).substr(firstSize));
    CHECK(upload.uploadedSize == png.size());

    upload = files.completeUpload(alice.id, upload.id, now);
    CHECK(upload.state == FileState::Ready && upload.sha256.size() == 64);
    CHECK(files.completeUpload(alice.id, upload.id, now).sha256 == upload.sha256);
    CHECK(std::filesystem::file_size(storage.readyPath(upload.id)) == png.size());

    expectError(ErrorCode::Forbidden, [&] {
        (void)files.download(charlie.id, upload.id);
    });
    const auto sent = chat.sendFile(alice.id, direct.id, "file-client-1",
                                    "头像原图", upload.id, now);
    CHECK(sent.inserted && sent.message.attachments.size() == 1);
    CHECK(sent.message.attachments.front().fileId == upload.id);
    const auto duplicate = chat.sendFile(alice.id, direct.id, "file-client-1",
                                         "头像原图", upload.id, now);
    CHECK(!duplicate.inserted && duplicate.message.id == sent.message.id);
    expectError(ErrorCode::Conflict, [&] {
        (void)chat.sendText(alice.id, direct.id, "file-client-1",
                            "different command", now);
    });

    const auto history = chat.history(bob.id, direct.id, 0, 10);
    CHECK(history.size() == 1 && history.front().attachments.size() == 1);
    const auto bobDownload = files.download(bob.id, upload.id);
    CHECK(bobDownload.file.sha256 == upload.sha256);

    const auto updated = files.useAsAvatar(alice.id, upload.id);
    CHECK(updated.avatarFileId && *updated.avatarFileId == upload.id);
    // 头像是“已登录用户可读”的公开资料；绑定前 Charlie 无权限，绑定后可读。
    CHECK(files.download(charlie.id, upload.id).file.id == upload.id);

    auto disguised = files.createUpload(alice.id, "not-image.png", "image/png",
                                        8, now);
    disguised = files.appendChunk(alice.id, disguised.id, 0, "notapng!");
    expectError(ErrorCode::InvalidArgument, [&] {
        (void)files.completeUpload(alice.id, disguised.id, now);
    });
    const auto failed = store.findFile(disguised.id);
    CHECK(failed && failed->state == FileState::Failed);
    expectError(ErrorCode::NotFound, [&] {
        (void)files.download(alice.id, disguised.id);
    });

    expectError(ErrorCode::InvalidArgument, [&] {
        (void)files.createUpload(alice.id, "../escape.png", "image/png", 8,
                                 now);
    });
    expectError(ErrorCode::InvalidArgument, [&] {
        (void)files.createUpload(alice.id, "program.bin",
                                 "application/x-msdownload", 32, now);
    });
}

} // namespace

int main()
{
    try
    {
        verifyFileLifecycle();
        std::cout << "phase11 file lifecycle tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "phase11 file lifecycle tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
