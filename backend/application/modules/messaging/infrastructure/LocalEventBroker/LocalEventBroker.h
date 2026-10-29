#pragma once
#include "backend/application/modules/messaging/ports/IEventBroker/IEventBroker.h"
namespace webserver::function3 {
// 单进程运行模式仍消费持久 Outbox/Inbox；明确标为 local，不伪装成 MQ 已连接。
class LocalEventBroker final : public IEventBroker {
public:
    explicit LocalEventBroker(BrokerConsumer consumer):consumer_(std::move(consumer)){}
    bool publish(const BrokerMessage&message) override;
private:BrokerConsumer consumer_;
};
}
