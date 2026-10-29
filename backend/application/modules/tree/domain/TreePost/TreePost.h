#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <optional>
#include <vector>

namespace webserver::function3 {
enum class PostState { Published, Hidden, Deleted };
enum class MediaKind { Image, Audio };
struct TreeMedia {
    Id file{};
    MediaKind kind{};
    std::uint64_t bytes{};
    Milliseconds duration{};
};
struct PublishPost {
    std::string requestId;
    std::string body;
    Id category{};
    std::string mood;
    std::vector<std::string> tags;
    std::vector<Id> files;
    bool treeChannel{true};
    bool profileChannel{true};
    std::string visibility{"members"}; // members=登录用户；friends=好友；private=自己。
};
struct TreePost {
    Id id{};
    Id author{};
    std::string authorName;
    PublishPost content;
    std::vector<TreeMedia> media;
    std::string canonicalRequest;
    PostState state{PostState::Published};
    Milliseconds createdAt{};
    std::uint64_t version{1};
    std::uint64_t likes{};
    std::uint64_t repliers{};
    bool likedByViewer{};
    bool canDelete{};
    bool deleteReasonRequired{};
    bool canHide{};
    bool favoritedByViewer{};
};
struct PostQuery {
    Id before{};
    std::size_t limit{20};
    std::optional<Id> author;
    std::optional<Id> category;
    std::string tag;
    std::string mood;
    std::optional<MediaKind> mediaKind;
    std::optional<Milliseconds> since,until; // [since, until)，与 ID 游标共同使用。
    bool profileOnly{}; // false=树频道；true=主页频道，必须同时提供 author。
    bool allChannels{}; // 本人/好友的主页汇总，必须指定作者；ACL 仍逐条生效。
    std::vector<Id> ids; // 可选有界批量读取，仍先执行频道/可见范围过滤再分页。
};
struct TreeCategory { Id id{}; std::string name; bool active{true}; };
}
