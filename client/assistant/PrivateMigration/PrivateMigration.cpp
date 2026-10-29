#include "PrivateMigration.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include "backend/application/modules/assistant/domain/PrivateArchive/PrivateArchive.h"
#include <fstream>
namespace nebula::client {
namespace j=boost::json;namespace http=boost::beast::http;using webserver::function3::LegacyPrivatePage;
namespace{
std::string field(const j::object&o,const char*key){const auto*v=o.if_contains(key);if(!v||!v->is_string())throw std::runtime_error("迁移字段缺失");return std::string(v->as_string());}
std::uint64_t number(const j::object&o,const char*key){const auto value=field(o,key);if(value.empty()||value.size()>19||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("无效迁移游标");return std::stoull(value);}
j::value accepted(const ProxyResponse&r){if(r.result_int()<200||r.result_int()>=300)throw std::runtime_error("服务器未接受迁移；可能版本尚未升级、来源变化、权限不足或包含尚未迁移的附件，原副本保留");return j::parse(r.body());}
LegacyPrivatePage verified(const j::object&o,const std::string&account){
    LegacyPrivatePage p;p.owner=number(o,"owner");p.kind=field(o,"kind");p.resource=number(o,"resource");p.after=number(o,"after");p.through=number(o,"through");p.upper=number(o,"upper");p.title=field(o,"title");p.checksum=field(o,"checksum");
    if(std::to_string(p.owner)!=account||(p.kind!="conversation"&&p.kind!="knowledge")||p.after>p.through||p.through>p.upper||p.title.size()>800)throw std::runtime_error("迁移来源或账号不匹配");
    const auto*attachments=o.if_contains("attachments"),*entries=o.if_contains("entries");if(!attachments||!attachments->is_bool()||!entries||!entries->is_array()||entries->as_array().size()>500)throw std::runtime_error("迁移分页格式无效");p.attachments=attachments->as_bool();auto previous=p.after;std::size_t bytes=0;
    for(const auto&item:entries->as_array()){if(!item.is_object())throw std::runtime_error("迁移条目格式无效");const auto&row=item.as_object();const auto id=number(row,"id");const auto role=field(row,"role"),body=field(row,"body");if(id<=previous||id>p.through||body.size()>16384||(p.kind=="knowledge"?role!="document":role!="user"&&role!="assistant"))throw std::runtime_error("迁移条目不满足有序边界");previous=id;bytes+=body.size();p.entries.push_back({id,role,body});}
    if(bytes>512*1024||sha256(privateArchiveMaterial(p))!=p.checksum)throw std::runtime_error("迁移摘要校验失败，不导入也不清除原副本");return p;
}
}
PrivateMigration::PrivateMigration(SqliteStore&store,PrivateHistory&history,LocalKnowledge&knowledge,std::filesystem::path root,UpstreamTransport&upstream):store_(store),history_(history),knowledge_(knowledge),root_(std::move(root)),upstream_(upstream){
    store_.execute("CREATE TABLE IF NOT EXISTS local_migration_threads(resource TEXT PRIMARY KEY,thread_id TEXT NOT NULL REFERENCES private_threads(id) ON DELETE CASCADE)");
    store_.execute("CREATE TABLE IF NOT EXISTS local_migration_pages(id TEXT PRIMARY KEY,kind TEXT NOT NULL,resource TEXT NOT NULL,after_cursor TEXT NOT NULL,through_cursor TEXT NOT NULL,checksum TEXT NOT NULL,file TEXT NOT NULL,file_hash TEXT NOT NULL,attachments INTEGER NOT NULL,purged INTEGER NOT NULL DEFAULT 0,UNIQUE(kind,resource,after_cursor,through_cursor,checksum))");
}
j::value PrivateMigration::verifyForImport(const j::object&archive,const std::string&account){const auto page=verified(archive,account);return j::object{{"checksum",page.checksum},{"entries",page.entries.size()}};}
j::value PrivateMigration::resources(const BoundServerSession&identity){ProxyRequest request{http::verb::get,"/api/ai/private/legacy",11};return accepted(upstream_.requestBound(identity,std::move(request)));}
j::value PrivateMigration::pages(){j::array result;for(const auto&r:store_.query("SELECT id,kind,resource,after_cursor,through_cursor,checksum,attachments,purged FROM local_migration_pages ORDER BY rowid DESC LIMIT 100"))result.push_back(j::object{{"id",*r[0]},{"kind",*r[1]},{"resource",*r[2]},{"after",*r[3]},{"through",*r[4]},{"checksum",*r[5]},{"attachments",*r[6]=="1"},{"purged",*r[7]=="1"},{"verified",true}});return result;}
j::value PrivateMigration::importPage(const BoundServerSession&identity,const j::object&request){
    const auto kind=field(request,"kind"),resource=field(request,"resourceId");const auto after=number(request,"after"),upper=number(request,"upper");if((kind!="conversation"&&kind!="knowledge")||resource.empty()||resource.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("无效迁移资源");
    ProxyRequest fetch{http::verb::get,"/api/ai/private/legacy/"+kind+"/"+resource+"?after="+std::to_string(after)+"&upper="+std::to_string(upper),11};auto archive=accepted(upstream_.requestBound(identity,std::move(fetch)));if(!archive.is_object())throw std::runtime_error("迁移响应无效");const auto page=verified(archive.as_object(),identity.account);
    if(page.kind!=kind||std::to_string(page.resource)!=resource||page.after!=after)throw std::runtime_error("导出资源与请求不符");
    const auto serialized=j::serialize(archive),fileHash=sha256(serialized),id=randomToken();const auto directory=root_/L"archives";createPrivateDirectory(directory);std::uintmax_t bytes=0;for(const auto&e:std::filesystem::directory_iterator(directory))if(e.is_regular_file())bytes+=e.file_size();if(bytes+serialized.size()>32*1024*1024)throw std::runtime_error("迁移原始归档达到32MiB上限，请先整理");
    std::string receipt=id;const auto prior=store_.query("SELECT id FROM local_migration_pages WHERE kind=? AND resource=? AND after_cursor=? AND through_cursor=? AND checksum=?",{kind,resource,std::to_string(after),std::to_string(page.through),page.checksum});if(!prior.empty())return j::object{{"receiptId",*prior[0][0]},{"through",std::to_string(page.through)},{"upper",std::to_string(page.upper)},{"verified",true},{"serverCopyDeleted",false}};
    const auto file=directory/(id+".json");{std::ofstream output(file,std::ios::binary);output.write(serialized.data(),serialized.size());output.close();if(!output)throw std::runtime_error("迁移归档写入失败");}
    try{store_.transact([&]{
        if(kind=="conversation"){
            auto rows=store_.query("SELECT thread_id FROM local_migration_threads WHERE resource=?",{resource});std::string thread;
            if(rows.empty()){thread=history_.createThread("friend","迁移的AI对话").id;store_.execute("INSERT INTO local_migration_threads VALUES(?,?)",{resource,thread});}else thread=*rows[0][0];
            for(const auto&e:page.entries)history_.append(thread,"legacy:"+resource+":"+std::to_string(e.id),e.role,e.body,"completed");
        }else{
            std::string body;std::size_t part=0;auto save=[&]{if(!body.empty()){knowledge_.importText("迁移资料 · "+std::to_string(page.resource)+" / "+std::to_string(after)+" / "+std::to_string(part++),"legacy-private-chunk-archive",body,false);body.clear();}};
            for(const auto&e:page.entries){if(body.size()+e.body.size()+2>200*1024)save();body+=e.body+"\n\n";}save();
        }
        store_.execute("INSERT INTO local_migration_pages(id,kind,resource,after_cursor,through_cursor,checksum,file,file_hash,attachments)VALUES(?,?,?,?,?,?,?,?,?)",{id,kind,resource,std::to_string(after),std::to_string(page.through),page.checksum,file.filename().string(),fileHash,page.attachments?"1":"0"});
    });}catch(...){std::error_code ignored;std::filesystem::remove(file,ignored);throw;}
    return j::object{{"receiptId",receipt},{"through",std::to_string(page.through)},{"upper",std::to_string(page.upper)},{"entries",page.entries.size()},{"verified",true},{"attachments",page.attachments},{"serverCopyDeleted",false}};
}
j::value PrivateMigration::purge(const BoundServerSession&identity,const std::string&id,bool confirmed){
    if(!confirmed)throw std::runtime_error("必须单独确认是否清除服务器上的已核验文字副本");const auto rows=store_.query("SELECT file,file_hash,purged FROM local_migration_pages WHERE id=?",{id});if(rows.empty())throw std::runtime_error("迁移收据不存在");const auto name=std::filesystem::path(*rows[0][0]);if(name.has_parent_path())throw std::runtime_error("无效归档文件索引");const auto file=root_/L"archives"/name;
    if(!std::filesystem::is_regular_file(file)||std::filesystem::file_size(file)>2*1024*1024)throw std::runtime_error("本机归档缺失，不会清除原副本");std::ifstream input(file,std::ios::binary);const auto serialized=std::string(std::istreambuf_iterator<char>(input),{});if(sha256(serialized)!=*rows[0][1])throw std::runtime_error("本机归档被修改，不会清除原副本");auto parsed=j::parse(serialized);const auto page=verified(parsed.as_object(),identity.account);if(page.attachments)throw std::runtime_error("这一页含附件，请先独立迁移文件，当前保留服务器副本");
    if(*rows[0][2]=="1")return j::object{{"purged",true},{"duplicate",true}};
    ProxyRequest request{http::verb::post,"/api/ai/private/legacy/"+page.kind+"/"+std::to_string(page.resource)+"/purge",11};request.set(http::field::content_type,"application/json");request.body()=j::serialize(j::object{{"after",std::to_string(page.after)},{"through",std::to_string(page.through)},{"checksum",page.checksum},{"confirmed","true"}});request.prepare_payload();const auto result=accepted(upstream_.requestBound(identity,std::move(request)));store_.execute("UPDATE local_migration_pages SET purged=1 WHERE id=?",{id});return result;
}
}
