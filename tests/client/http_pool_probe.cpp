#include "client/companion/HttpConnectionPool/HttpConnectionPool.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
using namespace nebula::client;
namespace http=boost::beast::http;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("confirmed certificate SHA256 required; use only isolated TLS fixture on 18444");
    ServerTrust trust{"192.168.239.135",18444,argv[1]};trust.connectHost="127.0.0.1";
    HttpConnectionPool pool(trust);
    auto call=[&](std::string path,http::verb verb=http::verb::get,std::uint64_t epoch=0){
        http::request<http::string_body> request{verb,path,11};request.set(http::field::host,trust.host);request.body()="fixture";request.prepare_payload();
        return pool.request(std::move(request),epoch);
    };
    for(int i=0;i<20;i++)check(call("/keep").body()=="ok","keepalive response mismatch");
    auto stats=pool.statistics();check(stats.connected==1&&stats.reused==19&&stats.idle==1,"short reads did not reuse one TLS connection");
    call("/close");check(pool.statistics().idle==0,"Connection:close returned to pool");
    call("/stale");call("/keep");check(pool.statistics().readRetries==1,"stale GET must reconnect once");
    bool failed=false;try{call("/ambiguous",http::verb::post);}catch(...){failed=true;}
    check(failed&&pool.statistics().readRetries==1,"ambiguous POST must not automatically replay");
    check(call("/writes").body()=="1","server observed duplicate uncertain write");
    failed=false;try{call("/large");}catch(...){failed=true;}check(failed&&pool.statistics().idle==0,"oversized response connection must be discarded");
    call("/keep");pool.invalidate(1);check(pool.statistics().idle==0,"identity switch retained idle connection");
    call("/keep",http::verb::get,0);check(pool.statistics().idle==0,"late old epoch connection entered new identity pool");
    std::atomic_bool workerFailed=false;
    std::thread worker([&]{try{call("/hold",http::verb::get,1);}catch(...){workerFailed=true;}});
    for(int i=0;i<200&&pool.statistics().active==0;i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pool.invalidate(2);worker.join();check(!workerFailed&&pool.statistics().idle==0,"active old lease returned after invalidation");
    // 所有线程同时进入长读取，超过8个只能明确拒绝，不能额外建连接。
    std::atomic_bool start=false;std::atomic_int rejected=0;
    std::vector<std::thread> threads;
    for(int i=0;i<16;i++)threads.emplace_back([&]{while(!start)std::this_thread::yield();try{call("/hold",http::verb::get,2);}catch(...){++rejected;}});
    start=true;for(auto& t:threads)t.join();check(rejected>=8&&pool.statistics().idle<=8&&pool.statistics().active==0,"connection budget or permit recovery failed");
    // 过期租约由下一次获取清理，测试不修改时钟或缩小生产超时。
    std::this_thread::sleep_for(std::chrono::milliseconds(10100));
    check(pool.statistics().idle==0,"idle expiry not enforced");check(call("/keep",http::verb::get,2).body()=="ok","fresh request after expiry failed");
    stats=pool.statistics();std::cout<<"{\"passed\":true,\"connected\":"<<stats.connected<<",\"reused\":"<<stats.reused<<",\"safeReadRetries\":"<<stats.readRetries<<",\"rejectedOverBudget\":"<<rejected<<",\"active\":"<<stats.active<<",\"idle\":"<<stats.idle<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
