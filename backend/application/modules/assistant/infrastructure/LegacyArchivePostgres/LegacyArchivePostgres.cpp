#include "LegacyArchivePostgres.h"
#include <algorithm>
namespace webserver::function3 {
namespace{std::string n(Id value){return std::to_string(value);}const std::string marker="[private content migrated to the user device]";}
std::vector<LegacyPrivateResource>LegacyArchivePostgres::resources(Id owner){std::vector<LegacyPrivateResource>result;
    for(const auto&r:sql_.query("SELECT c.id,COALESCE(NULLIF(c.title,''),peer.display_name) FROM conversations c JOIN conversation_members mine ON mine.conversation_id=c.id AND mine.user_id=$1 JOIN conversation_members other ON other.conversation_id=c.id AND other.user_id<>$1 JOIN users peer ON peer.id=other.user_id WHERE c.kind='direct' AND c.state='active' AND peer.ai_account AND (SELECT count(*) FROM conversation_members WHERE conversation_id=c.id)=2 ORDER BY c.id LIMIT 200",{n(owner)}))result.push_back({"conversation",std::stoull(r[0]),r[1]});
    for(const auto&r:sql_.query("SELECT id,title FROM knowledge_documents WHERE owner_id=$1 AND scope='private' AND state='ready' ORDER BY id LIMIT 200",{n(owner)}))result.push_back({"knowledge",std::stoull(r[0]),r[1]});return result;
}
LegacyPrivatePage LegacyArchivePostgres::read(Id owner,std::string kind,Id resource,Id after,Id upper,bool exact){
    sql_.requireTransaction();LegacyPrivatePage result;result.owner=owner;result.kind=kind;result.resource=resource;result.after=after;SqlRows rows;
    if(kind=="conversation"){
        const auto current=sql_.query("SELECT c.next_sequence-1,c.title FROM conversations c WHERE c.id=$1 AND c.kind='direct' AND c.state='active' AND EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=c.id AND user_id=$2) AND (SELECT count(*) FROM conversation_members WHERE conversation_id=c.id)=2 AND EXISTS(SELECT 1 FROM conversation_members m JOIN users u ON u.id=m.user_id WHERE m.conversation_id=c.id AND u.id<>$2 AND u.ai_account) FOR UPDATE OF c",{n(resource),n(owner)});
        if(current.empty())throw BusinessError(Failure::NotFound,"only your two-member AI conversation can be migrated");result.title=current[0][1].empty()?"旧AI对话":current[0][1];const auto latest=std::stoull(current[0][0]);result.upper=upper?std::min<Id>(upper,latest):latest;
        rows=sql_.query("SELECT m.conversation_seq,CASE WHEN m.sender_id=$2 THEN 'user' ELSE 'assistant' END,CASE WHEN m.state='recalled' THEN '[已撤回]' ELSE m.body END,EXISTS(SELECT 1 FROM message_attachments WHERE message_id=m.id) FROM messages m WHERE m.conversation_id=$1 AND m.conversation_seq>$3 AND m.conversation_seq<=$4 ORDER BY m.conversation_seq LIMIT 501 FOR UPDATE OF m",{n(resource),n(owner),n(after),n(result.upper)});
    }else{
        const auto current=sql_.query("SELECT title FROM knowledge_documents WHERE id=$1 AND owner_id=$2 AND scope='private' AND state IN('ready','deleted') FOR UPDATE",{n(resource),n(owner)});if(current.empty())throw BusinessError(Failure::NotFound,"private knowledge is not owned by requester");result.title=current[0][0];
        const auto latest=std::stoull(sql_.query("SELECT COALESCE(max(ordinal)+1,0) FROM knowledge_chunks WHERE document_id=$1",{n(resource)})[0][0]);result.upper=upper?std::min<Id>(upper,latest):latest;
        rows=sql_.query("SELECT ordinal+1,'document',content,'f' FROM knowledge_chunks WHERE document_id=$1 AND ordinal+1>$2 AND ordinal+1<=$3 ORDER BY ordinal LIMIT 501 FOR UPDATE",{n(resource),n(after),n(result.upper)});
    }
    if(after>result.upper)throw BusinessError(Failure::Invalid,"archive cursor exceeds source snapshot");
    std::size_t bytes=0;bool limited=false;for(const auto&r:rows){if(result.entries.size()>=500||bytes+r[2].size()>512*1024){limited=true;break;}bytes+=r[2].size();result.entries.push_back({std::stoull(r[0]),r[1],r[2]});result.attachments|=r[3]=="t";}
    result.through=limited&&!result.entries.empty()?result.entries.back().id:result.upper;
    if(exact&&result.through!=upper)throw BusinessError(Failure::Conflict,"archive page changed or exceeds verification limit");return result;
}
LegacyPrivatePage LegacyArchivePostgres::exportPage(Id owner,std::string kind,Id resource,Id after,Id upper){LegacyPrivatePage result;sql_.transact([&]{result=read(owner,std::move(kind),resource,after,upper,false);});return result;}
void LegacyArchivePostgres::purgePage(Id owner,std::string kind,Id resource,Id after,Id through,std::string checksum,const std::function<std::string(const LegacyPrivatePage&)>&digest){
    sql_.transact([&]{
        const auto previous=sql_.query("SELECT checksum FROM private_archive_purge_receipts WHERE user_id=$1 AND kind=$2 AND resource_id=$3 AND after_cursor=$4 AND through_cursor=$5",{n(owner),kind,n(resource),n(after),n(through)});
        if(!previous.empty()){if(previous[0][0]!=checksum)throw BusinessError(Failure::Conflict,"purge receipt payload changed");return;}
        const auto page=read(owner,kind,resource,after,through,true);
        // 另一条相同确认可能在本次等待行锁期间已经提交，不能把它误报为内容变化。
        const auto duplicate=sql_.query("SELECT checksum FROM private_archive_purge_receipts WHERE user_id=$1 AND kind=$2 AND resource_id=$3 AND after_cursor=$4 AND through_cursor=$5",{n(owner),kind,n(resource),n(after),n(through)});
        if(!duplicate.empty()){if(duplicate[0][0]!=checksum)throw BusinessError(Failure::Conflict,"purge receipt payload changed");return;}
        if(page.attachments)throw BusinessError(Failure::Invalid,"this page contains attachments; migrate files separately and keep server copy");
        if(digest(page)!=checksum)throw BusinessError(Failure::Conflict,"source changed after export; re-export before deleting");
        if(kind=="conversation"){
            const auto removed=sql_.query("UPDATE messages SET body=$5,state='recalled',recalled_by=$2,recalled_at=CURRENT_TIMESTAMP WHERE conversation_id=$1 AND conversation_seq>$3 AND conversation_seq<=$4 RETURNING id",{n(resource),n(owner),n(after),n(through),marker});
            for(const auto&r:removed)sql_.appendLegacyEvent(events_,{0,"message.recalled","message",std::stoull(r[0]),n(resource),std::chrono::system_clock::now(),{}});
        }else{
            sql_.query("UPDATE knowledge_chunks SET content=$4 WHERE document_id=$1 AND ordinal+1>$2 AND ordinal+1<=$3",{n(resource),n(after),n(through),marker});
            sql_.query("UPDATE knowledge_documents SET state='deleted',failure_detail='explicitly migrated to local device' WHERE id=$1",{n(resource)});
        }
        sql_.query("INSERT INTO private_archive_purge_receipts(user_id,kind,resource_id,after_cursor,through_cursor,checksum)VALUES($1,$2,$3,$4,$5,$6)",{n(owner),kind,n(resource),n(after),n(through),checksum});
    });
}
}
