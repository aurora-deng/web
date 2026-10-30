#include "backend/application/storage/memory/MemoryWorkspaceRepository/MemoryWorkspaceRepository.h"
#include "backend/application/storage/memory/MemoryContactsRepository/MemoryContactsRepository.h"
#include "backend/application/storage/memory/MemoryMessageWriteRepository/MemoryMessageWriteRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
namespace { std::string lowerText(std::string_view text){std::string value(text);for(char&c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return value;} }
std::vector<webserver::chat::UserCard> MemoryWorkspaceRepository::searchUsers(const storage_detail::MemoryState &memory, UserId viewer, std::string_view query, UserId after, std::size_t limit)
{std::vector<webserver::chat::UserCard>result;const auto prefix=lowerText(std::string(query));
 for(const auto&[id,user]:memory.users){if(user.disabled||id==viewer||id<=after||MemoryContactsRepository::blockedEitherDirection(memory, viewer,id))continue;
 if(!lowerText(user.username).starts_with(prefix)&&!lowerText(user.displayName).starts_with(prefix))continue;
 bool pending=false;for(const auto&[_,request]:memory.friendRequests)if(request.state==FriendRequestState::Pending&&((request.senderId==viewer&&request.receiverId==id)||(request.senderId==id&&request.receiverId==viewer)))pending=true;
 // Search is a minimal directory, not permission to expose profile media.
 result.push_back({id,user.username,user.displayName,std::nullopt,user.aiAccount,MemoryContactsRepository::areFriends(memory, viewer,id),pending});}
 std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.id<b.id;});if(result.size()>limit)result.resize(limit);return result;
}

std::vector<Message> MemoryWorkspaceRepository::searchVisibleMessages(const storage_detail::MemoryState &memory, UserId viewer, ConversationId conversation, std::string_view query, MessageId before, std::size_t limit)
{std::vector<Message>result;const auto needle=lowerText(std::string(query));
 for(const auto&[id,message]:memory.messages){if((before&&id>=before)||(conversation&&message.conversationId!=conversation)||message.state!=MessageState::Active||!memory.members.contains({message.conversationId,viewer}))continue;
 if(memory.conversations.at(message.conversationId).state!=ConversationState::Active||lowerText(message.body).find(needle)==std::string::npos)continue;result.push_back(MemoryMessageWriteRepository::hydrateMessageLocked(memory, message));}
 std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.id>b.id;});if(result.size()>limit)result.resize(limit);return result;
}

webserver::chat::ConversationPreference MemoryWorkspaceRepository::preference(const storage_detail::MemoryState &memory, UserId user, ConversationId conversation)
{if(auto i=memory.conversationPreferences.find({conversation,user});i!=memory.conversationPreferences.end())return i->second;
 bool muted=false;auto c=memory.conversations.find(conversation);if(c!=memory.conversations.end()&&c->second.kind==ConversationKind::Direct)for(const auto&[key,member]:memory.members)if(key.first==conversation&&key.second!=user)muted=MemoryContactsRepository::contactSettings(memory, user,member.userId).notificationsMuted;
 return{conversation,user,muted,0};
}

webserver::chat::PreferenceWrite MemoryWorkspaceRepository::writePreference(storage_detail::MemoryState &memory, UserId user, ConversationId conversation, bool muted)
{if(!memory.members.contains({conversation,user}))throw std::runtime_error("preference requires membership");
 auto it=memory.conversationPreferences.find({conversation,user});if(it!=memory.conversationPreferences.end()&&it->second.muted==muted)return{it->second,false};
 auto&value=memory.conversationPreferences[{conversation,user}];value={conversation,user,muted,value.version+1};return{value,true};
}

}
