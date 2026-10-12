#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"
#include "tests/phase11/unit/RepositoryContract.h"

#include <iostream>
#include <stdexcept>

using namespace webserver::phase11;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Operation> void error(ErrorCode code, Operation operation)
{
    try { operation(); }
    catch (const ApplicationError &failure)
    {
        require(failure.code() == code, "unexpected application error");
        return;
    }
    throw std::runtime_error("operation did not reject");
}

int main()
{
    const auto now = TimePoint{std::chrono::seconds{1000}};
    InMemoryStore contract;
    test::runRepositoryContract({contract, contract, contract, contract,
                                contract, contract, contract}, now);
    InMemoryStore store;
    ChatService chat(store, store, store, store, store, store, store);
    const auto owner = store.create({0, "page-owner", "Owner", {}, "hash", false, false, now});
    const auto admin = store.create({0, "page-admin", "Admin", {}, "hash", false, false, now});
    const auto member = store.create({0, "page-member", "Member", {}, "hash", false, false, now});
    const auto outsider = store.create({0, "page-outsider", "Outsider", {}, "hash", false, false, now});
    const auto group = chat.createGroupConversation(owner.id, "Pages", {admin.id, member.id}, now);
    chat.setGroupMemberRole(owner.id, group.id, admin.id, MemberRole::Administrator, now);
    for (int index = 1; index <= 231; ++index)
        chat.sendText(member.id, group.id, "msg-" + std::to_string(index), "body", now);
    const auto latest = chat.historyBefore(admin.id, group.id, 0, 100);
    require(latest.front().sequence == 132 && latest.back().sequence == 231, "wrong latest page");
    const auto before = chat.historyBefore(admin.id, group.id, 132, 100);
    require(before.front().sequence == 32 && before.back().sequence == 131, "wrong previous page");
    error(ErrorCode::Forbidden, [&] { chat.historyBefore(outsider.id, group.id, 0, 100); });
    error(ErrorCode::InvalidArgument, [&] { chat.historyBefore(admin.id, group.id, 0, 501); });
    error(ErrorCode::InvalidArgument, [&] { chat.ownMessage(member.id, group.id, ""); });
    error(ErrorCode::Forbidden, [&] { chat.ownMessage(outsider.id, group.id, "msg-231"); });
    require(!chat.ownMessage(admin.id, group.id, "msg-231"), "lookup disclosed another sender's key");

    // 模拟提交完成、ACK 丢失、发送者随后被禁言。查询仍能确认原结果；不需要再次写入。
    chat.setGroupMemberMute(admin.id, group.id, member.id, now + std::chrono::minutes(10), now);
    const auto persisted = chat.ownMessage(member.id, group.id, "msg-231");
    require(persisted && persisted->sequence == 231, "lost ACK could not be reconciled");
    chat.setGroupMemberMute(admin.id, group.id, member.id, std::nullopt, now);
    require(!chat.sendText(member.id, group.id, "msg-231", "body", now).inserted,
            "retry duplicated committed message");
    error(ErrorCode::Conflict, [&] { chat.sendText(member.id, group.id, "msg-231", "changed", now); });

    const auto recalled = chat.recallMessage(admin.id, persisted->id, now + std::chrono::seconds(20));
    require(recalled.changed, "admin could not recall member message");
    require(chat.ownMessage(member.id, group.id, "msg-231")->state == MessageState::Recalled,
            "lookup restored recalled content state");
    require(chat.history(admin.id, group.id, 230, 100).front().state == MessageState::Recalled,
            "offline history lost recall state");
    const auto ownerMessage = chat.sendText(owner.id, group.id, "owner-message", "owner body", now);
    error(ErrorCode::Forbidden, [&] { chat.recallMessage(admin.id, ownerMessage.message.id, now); });
    const auto adminMessage = chat.sendText(admin.id, group.id, "admin-message", "admin body", now);
    error(ErrorCode::Forbidden, [&] { chat.recallMessage(member.id, adminMessage.message.id, now); });
    error(ErrorCode::Conflict, [&] { chat.recallMessage(owner.id, adminMessage.message.id,
                                                     now + std::chrono::minutes(3)); });
    require(chat.recallMessage(owner.id, adminMessage.message.id, now).changed,
            "owner could not recall admin message");
    chat.markRead(admin.id, group.id, 233, now);
    chat.markRead(admin.id, group.id, 100, now);
    require(store.findMember(group.id, admin.id)->lastReadSequence == 233, "late read regressed cursor");
    std::cout << "chat reliability: repository, pagination, reconciliation, recall and read passed\n";
}
