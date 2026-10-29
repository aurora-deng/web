#pragma once
#include "backend/application/modules/messaging/ports/IEventBroker/IEventBroker.h"
#include <memory>
namespace webserver::function3 {
struct RocketMqConfig {std::string endpoint;std::string consumerGroup{"Function3Delivery"};bool tls{};};
class RocketMqBroker final:public IEventBroker {
public:
    RocketMqBroker(RocketMqConfig config,BrokerConsumer consumer);
    ~RocketMqBroker();
    bool publish(const BrokerMessage &message)override;
private:struct Impl;std::unique_ptr<Impl>impl_;
};
}
