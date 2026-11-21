#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <locale>
#include <string>
#include <sstream>
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
    inline constexpr std::array<unsigned long long, 14> databaseBounds{
        250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000,
        1000000, 2500000, 10000000};
    inline std::array<std::atomic_ullong, databaseBounds.size()+1> databaseBins{};
    inline void observe(Stage stage,unsigned long long micros){
        auto&c=counters[stage];
        ++c.count;
        c.microseconds+=micros;
        auto old=c.maximum.load();
        while(old<micros&&!c.maximum.compare_exchange_weak(old,micros)){
        };
        if (stage == Database) {
            const auto bin = std::lower_bound(databaseBounds.begin(), databaseBounds.end(), micros)
                - databaseBounds.begin();
            databaseBins[static_cast<std::size_t>(bin)].fetch_add(1, std::memory_order_relaxed);
        }
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
        std::ostringstream histogram;
        histogram.imbue(std::locale::classic());
        histogram << "# TYPE nebula_database_operation_seconds histogram\n";
        unsigned long long count = 0;
        for (std::size_t index = 0; index < databaseBins.size(); ++index) {
            count += databaseBins[index].load(std::memory_order_relaxed);
            histogram << "nebula_database_operation_seconds_bucket{le=\"";
            if (index == databaseBounds.size()) histogram << "+Inf";
            else histogram << databaseBounds[index] / 1e6;
            histogram << "\"} " << count << '\n';
        }
        histogram << "nebula_database_operation_seconds_count " << count << '\n'
                  << "nebula_database_operation_seconds_sum "
                  << counters[Database].microseconds.load(std::memory_order_relaxed) / 1e6 << '\n';
        return result + histogram.str();
    }
}
