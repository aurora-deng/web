#pragma once
#include "backend/application/ports/IMessageRepository/IMessageRepository.h"
#include "backend/application/modules/messaging/ports/IEventBroker/IEventBroker.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/application/business/OutboxDispatcher/OutboxDispatcher.h"
#include "backend/application/modules/messaging/ports/IMessagingOperations/IMessagingOperations.h"
#include <atomic>
namespace webserver::function3 {
class PostgresEventPipeline final:public IMessagingOperations {
public:
    explicit PostgresEventPipeline(SqlContext &sql):sql_(sql){}
    bool publishOne(IEventBroker &broker);
    std::size_t publishBatch(IEventBroker &broker,std::size_t limit=4);
    /** Inbox 与后续持久任务在一个事务提交，提交后才允许返回消费成功。 */
    bool consume(const BrokerMessage &message,const std::function<void(std::string,std::string)> &notify,
        const std::function<void(const std::map<std::string,std::string>&)>&durable={});
    bool retry(Id event)override;
    std::vector<QueueState>status()const override;
    std::vector<DeadEvent>dead(Id before)const override;
    std::vector<DeliveryFailure>failures(Id before)const override;
    std::uint64_t accepted()const{return accepted_;}
    std::uint64_t duplicates()const{return duplicates_;}
private:SqlContext &sql_;std::atomic<std::uint64_t>accepted_{},duplicates_{};
};
/** 把旧聊天 Outbox 送入同一 Broker，保留原来的聊天业务和持久 ACK。 */
class ChatMqSink final:public phase11::IOutboxEventSink {
public:
    ChatMqSink(SqlContext&sql,phase11::IMessageRepository&messages,std::function<void()> committed={}):sql_(sql),messages_(messages),committed_(std::move(committed)){}
    bool publish(const phase11::OutboxEvent &event)override;
private:SqlContext&sql_;phase11::IMessageRepository&messages_;std::function<void()>committed_;
};
}
