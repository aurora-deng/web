#include "ChatWorkspaceService.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include <algorithm>
namespace webserver::chat {
    namespace {
        void queryText(std::string&query){
            const auto start=query.find_first_not_of(" \t\r\n");
            query=start==std::string::npos?"":query.substr(start,query.find_last_not_of(" \t\r\n")-start+1);
            if(query.empty()||query.size()>256)throw ApplicationError(ErrorCode::InvalidArgument,"query must contain 1..256 bytes");
        }
        void pageSize(std::size_t limit,std::size_t max){
            if(!limit||limit>max)throw ApplicationError(ErrorCode::InvalidArgument,"page limit outside allowed range");
        }
    }
    void ChatWorkspaceService::active(UserId actor)const{
        auto user=users_.findUser(actor);
        if(!user||user->disabled)throw ApplicationError(ErrorCode::Unauthenticated,"active account required");
    }
    void ChatWorkspaceService::member(UserId actor,ConversationId id)const{
        active(actor);
        auto c=conversations_.findConversation(id);
        if(!c||c->state!=ConversationState::Active||!conversations_.findMember(id,actor))throw ApplicationError(ErrorCode::Forbidden,"active conversation membership required");
    }
    UserPage ChatWorkspaceService::users(UserId actor,UserDirectoryQuery input)const{
        return directory_.search(actor,std::move(input));
    }
    MessagePage ChatWorkspaceService::search(UserId actor,MessageSearchQuery input)const{
        auto&[conversation,query,before,limit]=input;
        active(actor);
        if(conversation)member(actor,conversation);
        queryText(query);
        pageSize(limit,100);
        // Repository filters membership/state in the same query; LIMIT is never applied before ACL.
        MessagePage page{
            workspace_.searchVisibleMessages(actor,conversation,query,before,limit+1),0
        };
        if(page.items.size()>limit){
            page.items.resize(limit);
            page.nextCursor=page.items.back().id;
        }
        return page;
    }
    ConversationPreference ChatWorkspaceService::preferences(UserId actor,ConversationId conversation)const{
        member(actor,conversation);
        return workspace_.preference(actor,conversation);
    }
    ConversationPreference ChatWorkspaceService::setPreferences(UserId actor,SetConversationPreference input,TimePoint now){
        const auto[conversation,muted]=input;
        auto transaction=db_.beginTransaction();
        member(actor,conversation);
        const auto result=workspace_.writePreference(actor,conversation,muted);
        if(result.changed){
            const auto payload="{\"type\":\"conversation.preferences\",\"conversationId\":\""+std::to_string(conversation)+"\",\"notificationsMuted\":"+(muted?"true":"false")+",\"version\":\""+std::to_string(result.value.version)+"\"}";
            outbox_.appendOutbox({
                0,"conversation.preferences","user",actor,payload,now,{
                }
            }
            );
        }
        transaction->commit();
        return result.value;
    }
    MessageContext ChatWorkspaceService::context(UserId actor,MessageContextQuery input)const{
        const auto[conversation,message,radius]=input;
        member(actor,conversation);
        pageSize(radius,50);
        const auto target=messages_.findMessage(message);
        if(!target||target->conversationId!=conversation)throw ApplicationError(ErrorCode::NotFound,"message not found in this conversation");
        MessageContext result{
            *target,messages_.messagesBefore(conversation,target->sequence,radius)
        };
        result.items.push_back(*target);
        const auto after=messages_.messagesAfter(conversation,target->sequence,radius);
        result.items.insert(result.items.end(),after.begin(),after.end());
        // Repository may retain audit data. Public use-case output never exposes a recalled body/media.
        const auto redact=[](Message&m){
            if(m.state==MessageState::Recalled){
                m.body.clear();
                m.attachments.clear();
            }
        };
        redact(result.target);
        for(auto&message:result.items)redact(message);
        return result;
    }
}
