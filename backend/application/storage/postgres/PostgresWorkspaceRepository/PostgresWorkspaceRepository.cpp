#include "PostgresWorkspaceRepository.h"
#include "WorkspaceRows.h"
#include "backend/application/storage/postgres/PostgresMessageRepository/PostgresMessageRepository.h"
#include <utility>
namespace webserver::phase11 {
    std::vector<chat::UserCard>PostgresWorkspaceRepository::users(PGconn*c,UserId viewer,std::string_view prefix,UserId after,std::size_t limit){
        // Directory cards deliberately omit profile media: profile display consent belongs to its own module.
        auto rows=pg::query(c,"SELECT u.id,u.username,u.display_name,NULL::bigint,u.ai_account,"
        "EXISTS(SELECT 1 FROM friendships f WHERE f.user_low_id=LEAST(u.id,$1::bigint) AND f.user_high_id=GREATEST(u.id,$1::bigint)),"
        "EXISTS(SELECT 1 FROM friend_requests r WHERE r.state='pending' AND ((r.sender_id=$1 AND r.receiver_id=u.id) OR (r.receiver_id=$1 AND r.sender_id=u.id))) "
        "FROM users u WHERE NOT u.disabled AND u.id<>$1 AND u.id>$3 AND (lower(u.username) LIKE lower($2) ESCAPE '!' OR lower(u.display_name) LIKE lower($2) ESCAPE '!') "
        "AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=$1 AND b.blocked_id=u.id) OR (b.blocker_id=u.id AND b.blocked_id=$1)) ORDER BY u.id LIMIT $4",
        {
            std::to_string(viewer),pg::escapeLike(prefix)+"%",std::to_string(after),std::to_string(limit)
        }
        );
        std::vector<chat::UserCard>result;
        for(int i=0;i<rows.rows();i++){
            workspace_rows::DirectoryRow row{std::stoull(rows.value(i,0)),rows.value(i,1),rows.value(i,2),
                rows.isNull(i,3)?std::nullopt:std::optional<std::uint64_t>{std::stoull(rows.value(i,3))},
                rows.value(i,4)=="t",rows.value(i,5)=="t",rows.value(i,6)=="t"};
            // Cross the DAO/view boundary explicitly without copying owned text again.
            result.push_back({row.id,std::move(row.username),std::move(row.displayName),row.avatar,row.ai,row.friendship,row.pending});
        }
        return result;
    }
    std::vector<Message>PostgresWorkspaceRepository::search(PGconn*c,UserId user,ConversationId conversation,std::string_view query,MessageId before,std::size_t limit){
        return PostgresMessageRepository::hydrate(c,pg::query(c,"SELECT "+PostgresMessageRepository::columns("m.")+" FROM messages m JOIN conversations c ON c.id=m.conversation_id JOIN conversation_members cm ON cm.conversation_id=m.conversation_id AND cm.user_id=$1 "
        "WHERE c.state='active' AND m.state='active' AND ($2::bigint=0 OR m.conversation_id=$2) AND ($3::bigint=0 OR m.id<$3) AND m.body ILIKE $4 ESCAPE '!' ORDER BY m.id DESC LIMIT $5",
        {
            std::to_string(user),std::to_string(conversation),std::to_string(before),"%"+pg::escapeLike(query)+"%",std::to_string(limit)
        }
        ));
    }
    chat::ConversationPreference PostgresWorkspaceRepository::preference(PGconn*c,UserId user,ConversationId conversation){
        const auto rows=pg::query(c,"SELECT COALESCE(p.notifications_muted,cp.notifications_muted,false),COALESCE(p.revision,0) FROM conversations c "
        "LEFT JOIN conversation_preferences p ON p.conversation_id=c.id AND p.user_id=$1 "
        "LEFT JOIN LATERAL(SELECT cp.notifications_muted FROM conversation_members pm JOIN contact_preferences cp ON cp.owner_id=$1 AND cp.contact_id=pm.user_id WHERE c.kind='direct' AND pm.conversation_id=c.id AND pm.user_id<>$1 ORDER BY pm.user_id LIMIT 1) cp ON true WHERE c.id=$2",
        {
            std::to_string(user),std::to_string(conversation)
        }
        );
        if(!rows.rows())throw ApplicationError(ErrorCode::NotFound,"conversation not found");
        return{
            conversation,user,rows.value(0,0)=="t",std::stoull(rows.value(0,1))
        };
    }
    chat::PreferenceWrite PostgresWorkspaceRepository::writePreference(PGconn*c,UserId user,ConversationId conversation,bool muted){
        auto rows=pg::query(c,"INSERT INTO conversation_preferences(conversation_id,user_id,notifications_muted,revision) VALUES($1,$2,$3,1) "
        "ON CONFLICT(conversation_id,user_id) DO UPDATE SET notifications_muted=EXCLUDED.notifications_muted,revision=conversation_preferences.revision+1,updated_at=CURRENT_TIMESTAMP "
        "WHERE conversation_preferences.notifications_muted IS DISTINCT FROM EXCLUDED.notifications_muted RETURNING revision",
        {
            std::to_string(conversation),std::to_string(user),muted?"true":"false"
        }
        );
        if(rows.rows())return{
            {
                conversation,user,muted,std::stoull(rows.value(0,0))
            }
            ,true
        };
        return{
            preference(c,user,conversation),false
        };
    }
}
