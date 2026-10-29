#pragma once
#include <functional>
#include <string>
namespace webserver::function3 {
struct BrokerMessage {std::string topic,group,key,body;bool fifo{true};};
using BrokerConsumer=std::function<bool(const BrokerMessage&)>;
class IEventBroker {
public:
    virtual ~IEventBroker()=default;
    /** 只由发布线程调用；true 表示 Broker 确认，绝不表示用户已送达或已读。 */
    virtual bool publish(const BrokerMessage &message)=0;
};
}
