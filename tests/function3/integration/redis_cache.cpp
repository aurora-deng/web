#include "backend/application/modules/cache/infrastructure/RedisReadCache/RedisReadCache.h"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace webserver::function3;
void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
int main(){
    const auto*port=std::getenv("FUNCTION3_REDIS_TEST_PORT");
    if(!port){std::cout<<"Set FUNCTION3_REDIS_TEST_PORT for an existing Redis; no service installed or started\n";return 77;}
    try{
        RedisCacheConfig config;config.port=std::stoul(port);config.prefix="nebula:function3:contract:";config.timeoutMs=150;
        RedisReadCache cache(config);cache.invalidate("catalog");
        auto first=cache.lookup("catalog");check(!first.value&&!first.generation.empty(),"empty generation");
        cache.store("catalog",first.generation,"{\"name\":\"生活\"}",1);
        check(cache.lookup("catalog").value=="{\"name\":\"生活\"}","real Redis roundtrip");
        cache.invalidate("catalog");cache.store("catalog",first.generation,"stale read",1);
        auto next=cache.lookup("catalog");check(!next.value&&next.generation!=first.generation,"old reader cannot refill a new generation");
        cache.store("catalog",next.generation,"fresh",1);check(cache.lookup("catalog").value=="fresh","new generation");
        const std::string large(64*1024,'x');cache.store("large","0",large,1);
        check(cache.lookup("large").value==large,"buffered RESP must preserve multi-chunk payloads");
        cache.store("empty","0","",1);check(cache.lookup("empty").value=="","empty value is distinct from a cache miss");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));check(!cache.lookup("catalog").value,"TTL must expire");
        config.port=1;RedisReadCache unavailable(config);auto begin=std::chrono::steady_clock::now();check(!unavailable.lookup("catalog").value,"failure returns miss");check(!unavailable.lookup("catalog").value,"open circuit returns miss");check(unavailable.metrics().errors==1&&unavailable.metrics().bypassed==1,"failure isolation metrics");check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"bounded failure");
        auto metrics=cache.metrics();check(metrics.hits>=2&&metrics.errors==0,"hit metrics");std::cout<<"Existing Redis GET/SETEX, generation invalidation, TTL and bounded outage fallback passed\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
