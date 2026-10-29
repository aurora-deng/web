#include "backend/application/modules/messaging/infrastructure/RocketMqBroker/RocketMqBroker.h"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <vector>
#include <algorithm>
using namespace webserver::function3;
int main(int argc,char**argv){try{
    std::mutex mutex;std::condition_variable ready;std::vector<std::string>received;int redeliveries=0;bool normal=false;
    const auto key=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    RocketMqBroker broker({argc>1?argv[1]:"127.0.0.1:20881","Function3Roundtrip",false},[&](const BrokerMessage&m){
        std::lock_guard lock(mutex);
        if(m.key==key+":normal"){normal=true;ready.notify_all();return true;}
        if(!m.key.starts_with(key+":"))return true;
        if(m.key==key+":0"&&redeliveries++==0){received.push_back(m.key);return false;} // 模拟副作用已提交但确认丢失。
        if(std::find(received.begin(),received.end(),m.key)==received.end())received.push_back(m.key);
        ready.notify_all();return true;
    });
    auto body=[&](int i){return "{\"id\":\""+std::to_string(std::stoull(key)+i)+"\",\"type\":\"diagnostic.roundtrip\",\"aggregate\":\"diagnostic\",\"aggregateId\":\"1\",\"version\":\"1\",\"payload\":\"{}\"}";};
    for(int i=0;i<3;++i)if(!broker.publish({"Function3Tree","roundtrip:"+key,key+":"+std::to_string(i),body(i),true}))throw std::runtime_error("broker did not acknowledge send");
    if(!broker.publish({"Function3Speech","",key+":normal",body(3),false}))throw std::runtime_error("normal task was not acknowledged");
    std::unique_lock lock(mutex);if(!ready.wait_for(lock,std::chrono::seconds(60),[&]{return received.size()>=3&&normal&&redeliveries>=2;}))throw std::runtime_error("consumer/redelivery timeout");
    std::vector<std::string>own;for(auto&s:received)if(s.starts_with(key+":"))own.push_back(s);
    if(own.size()!=3||own[0]!=key+":0"||own[1]!=key+":1"||own[2]!=key+":2")throw std::runtime_error("FIFO roundtrip order mismatch");
    lock.unlock();std::cout<<"Real RocketMQ FIFO order (3), NORMAL task and negative-ack redelivery passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
