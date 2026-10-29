#include "LocalKnowledge.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace nebula::client {
namespace {
std::vector<std::string>characters(std::string_view text){
    std::vector<std::string>result;
    for(std::size_t i=0;i<text.size();){const auto start=i;const auto c=static_cast<unsigned char>(text[i++]);
        int extra=0;std::uint32_t value=c,minimum=0;
        if(c>=0xc2&&c<=0xdf){extra=1;value=c&31;minimum=0x80;}else if(c>=0xe0&&c<=0xef){extra=2;value=c&15;minimum=0x800;}else if(c>=0xf0&&c<=0xf4){extra=3;value=c&7;minimum=0x10000;}else if(c>=0x80)throw std::runtime_error("资料必须是有效 UTF-8 文本");
        while(extra--){if(i==text.size())throw std::runtime_error("不完整的 UTF-8 文本");const auto n=static_cast<unsigned char>(text[i++]);if((n&0xc0)!=0x80)throw std::runtime_error("无效 UTF-8 文本");value=(value<<6)|(n&63);}
        if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff)||(!value))throw std::runtime_error("无效文本字符");
        result.emplace_back(text.substr(start,i-start));
    }return result;
}
}
std::map<std::string,int>LocalKnowledge::terms(std::string_view text){
    const auto chars=characters(text);std::map<std::string,int>result;std::string word,previous;
    auto flush=[&]{if(!word.empty()){++result[word];word.clear();}};
    for(const auto&c:chars){if(c.size()==1){const auto value=static_cast<unsigned char>(c[0]);if(std::isalnum(value))word+=static_cast<char>(std::tolower(value));else flush();previous.clear();}
        else{flush();++result[c];if(!previous.empty())++result[previous+c];previous=c;}}
    flush();return result;
}
LocalKnowledge::LocalKnowledge(SqliteStore&store):store_(store){
    store_.execute("CREATE TABLE IF NOT EXISTS local_documents(id TEXT PRIMARY KEY,title TEXT NOT NULL,source TEXT NOT NULL,checksum TEXT NOT NULL,enabled INTEGER NOT NULL,authorized INTEGER NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()),UNIQUE(checksum))");
    store_.execute("CREATE TABLE IF NOT EXISTS local_chunks(id TEXT PRIMARY KEY,document_id TEXT NOT NULL REFERENCES local_documents(id) ON DELETE CASCADE,ordinal INTEGER NOT NULL,body TEXT NOT NULL)");
    store_.execute("CREATE TABLE IF NOT EXISTS local_terms(term TEXT NOT NULL,chunk_id TEXT NOT NULL REFERENCES local_chunks(id) ON DELETE CASCADE,frequency INTEGER NOT NULL,PRIMARY KEY(term,chunk_id)) WITHOUT ROWID");
    store_.execute("CREATE INDEX IF NOT EXISTS local_chunks_document ON local_chunks(document_id,ordinal)");
    store_.execute("CREATE INDEX IF NOT EXISTS local_terms_chunk ON local_terms(chunk_id)");
    store_.execute("PRAGMA max_page_count=49152");store_.execute("PRAGMA journal_size_limit=8388608");
}
LocalDocument LocalKnowledge::importText(std::string title,std::string source,std::string body,bool authorized){
    if(title.empty()||title.size()>400||source.size()>400||body.empty()||body.size()>200*1024)throw std::runtime_error("资料标题或正文超出限制（文本最多200 KiB）");
    const auto chars=characters(body);const auto checksum=sha256(body);LocalDocument result{randomToken(),std::move(title),std::move(source),checksum,true,authorized};
    store_.transact([&]{
        auto existing=store_.query("SELECT id,title,source,enabled,authorized FROM local_documents WHERE checksum=?",{checksum});
        if(!existing.empty()){const auto&r=existing.front();result={*r[0],*r[1],*r[2],checksum,*r[3]=="1",*r[4]=="1"};return;}
        if(std::stoul(*store_.query("SELECT count(*) FROM local_documents")[0][0])>=200)throw std::runtime_error("本地资料已达200份上限，请先整理");
        store_.execute("INSERT INTO local_documents(id,title,source,checksum,enabled,authorized)VALUES(?,?,?,?,1,?)",{result.id,result.title,result.source,result.checksum,authorized?"1":"0"});
        std::string chunk;std::size_t ordinal=0;
        auto save=[&]{if(chunk.empty())return;if(ordinal>=256)throw std::runtime_error("资料分块超过上限");const auto id=result.id+":"+std::to_string(ordinal++);
            store_.execute("INSERT INTO local_chunks VALUES(?,?,?,?)",{id,result.id,std::to_string(ordinal),chunk});
            const auto tokens=terms(chunk);if(tokens.size()>1000)throw std::runtime_error("分块索引超过上限");
            for(const auto&[term,count]:tokens)store_.execute("INSERT INTO local_terms VALUES(?,?,?)",{term,id,std::to_string(count)});chunk.clear();};
        for(const auto&c:chars){if(chunk.size()+c.size()>900)save();chunk+=c;}save();
    });return result;
}
std::vector<LocalDocument>LocalKnowledge::documents(){std::vector<LocalDocument>result;for(const auto&r:store_.query("SELECT id,title,source,checksum,enabled,authorized FROM local_documents ORDER BY created DESC,id DESC LIMIT 200"))result.push_back({*r[0],*r[1],*r[2],*r[3],*r[4]=="1",*r[5]=="1"});return result;}
void LocalKnowledge::configure(const std::string&id,bool enabled,bool authorized){store_.execute("UPDATE local_documents SET enabled=?,authorized=? WHERE id=?",{enabled?"1":"0",authorized?"1":"0",id});}
void LocalKnowledge::remove(const std::string&id){store_.execute("DELETE FROM local_documents WHERE id=?",{id});}
std::vector<LocalCitation>LocalKnowledge::search(const std::string&query,std::size_t limit){
    if(query.empty()||query.size()>1024||limit==0||limit>6)throw std::runtime_error("检索请求超出限制");
    auto tokens=terms(query);if(tokens.empty())return{};if(tokens.size()>64)throw std::runtime_error("检索词过多，请缩短问题");
    Row args;std::string markers;for(const auto&[term,_]:tokens){if(!markers.empty())markers+=",";markers+="?";args.push_back(term);}args.push_back(std::to_string(limit));
    const auto rows=store_.query("SELECT d.id,c.id,d.title,c.body,SUM(t.frequency) AS score FROM local_terms t JOIN local_chunks c ON c.id=t.chunk_id JOIN local_documents d ON d.id=c.document_id WHERE d.enabled=1 AND d.authorized=1 AND t.term IN ("+markers+") GROUP BY c.id ORDER BY score DESC,c.id LIMIT ?",args);
    std::vector<LocalCitation>result;for(const auto&r:rows)result.push_back({*r[0],*r[1],*r[2],*r[3],std::stod(*r[4])});return result;
}
}
