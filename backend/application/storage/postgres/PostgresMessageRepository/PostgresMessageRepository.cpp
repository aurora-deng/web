#include "PostgresMessageRepository.h"
#include <algorithm>
#include <unordered_map>
namespace webserver::phase11 {
    namespace {
        TimePoint time(const pg::Result&r,int row,int column){
            return TimePoint{
                std::chrono::microseconds{
                    std::stoll(r.value(row,column))
                }
            };
        }
        std::optional<std::uint64_t>optionalId(const pg::Result&r,int row,int column){
            return r.isNull(row,column)?std::nullopt:std::optional<std::uint64_t>{
                std::stoull(r.value(row,column))
            };
        }
        Message parse(const pg::Result&r,int row){
            Message m;
            m.id=std::stoull(r.value(row,0));
            m.conversationId=std::stoull(r.value(row,1));
            m.sequence=std::stoull(r.value(row,2));
            m.senderId=std::stoull(r.value(row,3));
            m.clientMessageId=r.value(row,4);
            m.body=r.value(row,5);
            m.modelTrace.nodeId=optionalId(r,row,6);
            m.modelTrace.versionId=optionalId(r,row,7);
            m.modelTrace.adapterName=r.value(row,8);
            m.createdAt=time(r,row,9);
            m.state=r.value(row,10)=="recalled"?MessageState::Recalled:MessageState::Active;
            m.recalledBy=optionalId(r,row,11);
            if(!r.isNull(row,12))m.recalledAt=time(r,row,12);
            return m;
        }
    }
    std::string PostgresMessageRepository::columns(std::string p){
        return p+"id,"+p+"conversation_id,"+p+"conversation_seq,"+p+"sender_id,"+p+"client_message_id,"+p+"body,"+p+"model_node_id,"+p+"model_version_id,"+p+"adapter_name,(extract(epoch FROM "+p+"created_at)*1000000)::bigint,"+p+"state,"+p+"recalled_by,(extract(epoch FROM "+p+"recalled_at)*1000000)::bigint";
    }
    std::vector<Message>PostgresMessageRepository::hydrate(PGconn*c,const pg::Result&rows){
        std::vector<Message>result;
        std::unordered_map<MessageId,std::size_t>positions;
        std::string ids="{";
        for(int row=0;row<rows.rows();row++){
            auto message=parse(rows,row);
            positions.emplace(message.id,result.size());
            if(ids.size()>1)ids+=',';
            ids+=std::to_string(message.id);
            result.push_back(std::move(message));
        }
        if(result.empty())return result;
        ids+='}';
        // One attachment query for the entire page, not one query for every message.
        const auto files=pg::query(c,"SELECT a.message_id,f.id,f.original_name,f.media_type,f.expected_size,f.sha256 FROM message_attachments a JOIN file_objects f ON f.id=a.file_id WHERE a.message_id=ANY($1::bigint[]) AND f.state='ready' ORDER BY a.message_id,a.position",{
            ids
        }
        );
        for(int row=0;row<files.rows();row++){
            auto&message=result.at(positions.at(std::stoull(files.value(row,0))));
            message.attachments.push_back({
                std::stoull(files.value(row,1)),files.value(row,2),files.value(row,3),std::stoull(files.value(row,4)),files.value(row,5)
            }
            );
        }
        return result;
    }
    std::vector<Message>PostgresMessageRepository::after(PGconn*c,ConversationId id,std::uint64_t sequence,std::size_t limit){
        return hydrate(c,pg::query(c,"SELECT "+columns()+" FROM messages WHERE conversation_id=$1 AND conversation_seq>$2 ORDER BY conversation_seq LIMIT $3",{
            std::to_string(id),std::to_string(sequence),std::to_string(limit)
        }
        ));
    }
    std::vector<Message>PostgresMessageRepository::before(PGconn*c,ConversationId id,std::uint64_t sequence,std::size_t limit){
        auto result=hydrate(c,pg::query(c,"SELECT "+columns()+" FROM messages WHERE conversation_id=$1 AND ($2::bigint=0 OR conversation_seq<$2) ORDER BY conversation_seq DESC LIMIT $3",{
            std::to_string(id),std::to_string(sequence),std::to_string(limit)
        }
        ));
        std::reverse(result.begin(),result.end());
        return result;
    }
    std::vector<Message>PostgresMessageRepository::search(PGconn*c,ConversationId id,std::string_view query,std::size_t limit){
        return hydrate(c,pg::query(c,"SELECT "+columns()+" FROM messages WHERE conversation_id=$1 AND state='active' AND body ILIKE '%' || $2 || '%' ORDER BY conversation_seq LIMIT $3",{
            std::to_string(id),std::string(query),std::to_string(limit)
        }
        ));
    }
    std::optional<Message>PostgresMessageRepository::own(PGconn*c,ConversationId id,UserId user,std::string_view key){
        auto result=hydrate(c,pg::query(c,"SELECT "+columns()+" FROM messages WHERE conversation_id=$1 AND sender_id=$2 AND client_message_id=$3",{
            std::to_string(id),std::to_string(user),std::string(key)
        }
        ));
        return result.empty()?std::nullopt:std::optional<Message>{
            std::move(result.front())
        };
    }
    std::optional<Message>PostgresMessageRepository::find(PGconn*c,MessageId id){
        auto result=hydrate(c,pg::query(c,"SELECT "+columns()+" FROM messages WHERE id=$1",{
            std::to_string(id)
        }
        ));
        return result.empty()?std::nullopt:std::optional<Message>{
            std::move(result.front())
        };
    }
}
