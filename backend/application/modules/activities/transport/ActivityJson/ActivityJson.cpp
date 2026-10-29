#include "ActivityJson.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::function3 {
std::string activityJson(const TreePost&p) {
    using namespace http;
    const auto media=array(p.media,[&](const auto&m){return "{\"fileId\":"+id(m.file)+",\"kind\":"+quote(m.kind==MediaKind::Image?"image":"audio")+",\"url\":"+quote("/api/tree/posts/"+std::to_string(p.id)+"/media/"+std::to_string(m.file))+"}";});
    return "{\"id\":"+id(p.id)+",\"authorId\":"+id(p.author)+",\"authorName\":"+quote(p.authorName)+
        ",\"body\":"+quote(p.content.body)+",\"categoryId\":"+id(p.content.category)+",\"mood\":"+quote(p.content.mood)+
        ",\"tags\":"+array(p.content.tags,[](const auto&t){return quote(t);})+",\"media\":"+media+
        ",\"createdAt\":"+std::to_string(p.createdAt)+",\"version\":"+id(p.version)+",\"likes\":"+id(p.likes)+
        ",\"repliers\":"+id(p.repliers)+",\"liked\":"+(p.likedByViewer?"true":"false")+
        ",\"favorited\":"+(p.favoritedByViewer?"true":"false")+
        ",\"treeChannel\":"+(p.content.treeChannel?"true":"false")+
        ",\"profileChannel\":"+(p.content.profileChannel?"true":"false")+
        ",\"visibility\":"+quote(p.content.visibility)+
        ",\"capabilities\":{\"delete\":"+(p.canDelete?"true":"false")+
        ",\"deleteReasonRequired\":"+(p.deleteReasonRequired?"true":"false")+
        ",\"hide\":"+(p.canHide?"true":"false")+"}}";
}
}
