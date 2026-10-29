#include "backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h"
#include "backend/application/modules/tree/infrastructure/TreePostgresRepository/TreePostgresRepository.h"
#include "backend/application/modules/tree/infrastructure/TreeRankingSource/TreeRankingSource.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
using namespace webserver::function3;
int main(int argc,char**argv){try{
    if(argc!=2||std::string(argv[1])!="--isolated-test-only")throw std::runtime_error("explicit isolated capacity approval flag required; stop the isolated application first");
    const auto*connection=std::getenv("PHASE11_POSTGRES_CONNECTION");if(!connection)throw std::runtime_error("test connection required");
    SqlContext sql({connection,2,std::chrono::seconds(2)});
    if(sql.query("SELECT current_database()")[0][0]!="function3_phase11_test")throw std::runtime_error("refusing non-Function3 test database");
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto name="f3_capacity_"+std::to_string(now),category=sql.query("SELECT id FROM tree_categories WHERE active ORDER BY id LIMIT 1")[0][0];
    const auto user=sql.query("INSERT INTO users(username,display_name,password_hash) VALUES($1,'capacity fixture','test-only') RETURNING id",{name})[0][0];
    auto cleanup=[&]{for(;;){auto rows=sql.query("WITH batch AS(SELECT id FROM tree_posts WHERE author_id=$1 ORDER BY id LIMIT 1000) DELETE FROM tree_posts p USING batch WHERE p.id=batch.id RETURNING p.id",{user});if(rows.empty())break;}sql.query("DELETE FROM users WHERE id=$1",{user});};
    try{
        // 固定小批量建立夹具，避免一次写入十万行先耗尽单条SQL的安全超时。
        // 不改变被测查询、持久性或全局数据库参数。
        for(unsigned first=1;first<=100000;first+=2000)
            sql.query("INSERT INTO tree_posts(author_id,request_key,canonical_request,body,category_id,created_ms) SELECT $1,'capacity-'||g,'capacity-fixture','十万条容量夹具',$2,$3::bigint-g*10 FROM generate_series($4::integer,$5::integer)g",{user,category,std::to_string(now),std::to_string(first),std::to_string(first+1999)});
        TreePostgresRepository posts(sql);TreeRankingSource ranking(sql);PostQuery query;query.author=std::stoull(user);query.limit=24;
        const auto before=std::chrono::steady_clock::now();Id previous=~Id{};std::size_t seen=0;
        // 抽取连续二十分区及最老边界；不把抽样声称为十万条逐条遍历。
        for(int page=0;page<20;++page){auto rows=posts.posts(query,std::stoull(user));if(rows.size()!=24)throw std::runtime_error("partition size");for(auto&p:rows){if(p.id>=previous)throw std::runtime_error("exclusive cursor order");previous=p.id;++seen;}query.before=rows.back().id;}
        const auto listMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
        const auto minimum=std::stoull(sql.query("SELECT min(id) FROM tree_posts WHERE author_id=$1",{user})[0][0]);query.before=minimum+10;
        auto oldest=posts.posts(query,std::stoull(user));if(oldest.size()!=10||oldest.back().id!=minimum)throw std::runtime_error("oldest partition boundary");
        const auto rankStart=std::chrono::steady_clock::now();const auto top=ranking.topCandidates(now);if(top.size()>10)throw std::runtime_error("rank output bound");
        const auto rankMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-rankStart).count();
        rusage usage{};getrusage(RUSAGE_SELF,&usage);
        std::cout<<"{\"databaseFixtureRows\":100000,\"sampledPartitions\":20,\"sampledPosts\":"<<seen<<",\"maxPartitionRows\":24,\"oldestBoundary\":true,\"totalListMs\":"<<listMs<<",\"rankMs\":"<<rankMs<<",\"testProcessPeakRssKiB\":"<<usage.ru_maxrss<<"}\n";
        cleanup();
    }catch(...){cleanup();throw;}
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
