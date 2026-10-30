#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <string>
namespace webserver::business_metrics {
    enum Stage:std::size_t{
        Database,Queue,Delivery,CommitWake,OutboxCycle,Stages
    };
    struct Counter{
        std::atomic<unsigned long long>count{
        }
        ,microseconds{
        }
        ,maximum{
        };
    };
    inline std::array<Counter,Stages> counters;
    inline void observe(Stage stage,unsigned long long micros){
        auto&c=counters[stage];
        ++c.count;
        c.microseconds+=micros;
        auto old=c.maximum.load();
        while(old<micros&&!c.maximum.compare_exchange_weak(old,micros)){
        };
    }
    struct Timer{
        Stage stage;
        std::chrono::steady_clock::time_point start{
            std::chrono::steady_clock::now()
        };
        ~Timer(){
            observe(stage,std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
        }
    };
    inline std::string text(){
        std::string result;
        const char*names[]={
            "database","queue_wait","outbox_event_age","commit_notify_to_dispatch","outbox_cycle"
        };
        for(std::size_t i=0;i<Stages;i++){
            const std::string name="webserver_business_"+std::string(names[i]);
            result+=name+"_count "+std::to_string(counters[i].count.load())+"\n"+name+"_microseconds_total "+std::to_string(counters[i].microseconds.load())+"\n"+name+"_microseconds_max "+std::to_string(counters[i].maximum.load())+"\n";
        }
        return result;
    }
}
