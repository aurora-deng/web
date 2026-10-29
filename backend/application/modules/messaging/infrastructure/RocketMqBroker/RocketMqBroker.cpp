#include "RocketMqBroker.h"
#include <rocketmq/FifoProducer.h>
#include <rocketmq/Producer.h>
#include <rocketmq/PushConsumer.h>
#include <rocketmq/Logger.h>
#include <future>
#include <mutex>
#include <atomic>
namespace webserver::function3 {
struct RocketMqBroker::Impl {
    std::shared_ptr<std::atomic<std::size_t>> pending{std::make_shared<std::atomic<std::size_t>>(0)};
    rocketmq::FifoProducer fifo;
    rocketmq::Producer normal;
    rocketmq::PushConsumer consumer;
    Impl(RocketMqConfig config,BrokerConsumer onMessage)
        :fifo(rocketmq::FifoProducer::newBuilder().withConfiguration(configuration(config)).withTopics({"Function3Tree","Function3Chat"}).withConcurrency(2).build()),
         normal(rocketmq::Producer::newBuilder().withConfiguration(configuration(config)).withTopics({"Function3Speech"}).build()),
         consumer(rocketmq::PushConsumer::newBuilder().withConfiguration(configuration(config)).withGroup(config.consumerGroup).withConsumeThreads(2)
            .subscribe("Function3Tree",rocketmq::FilterExpression("*"))
            .subscribe("Function3Chat",rocketmq::FilterExpression("*"))
            .subscribe("Function3Speech",rocketmq::FilterExpression("*"))
            .withListener([onMessage=std::move(onMessage)](const rocketmq::Message&m){
                try{return onMessage({m.topic(),m.group(),m.keys().empty()?m.id():m.keys()[0],m.body(),m.topic()!="Function3Speech"})?rocketmq::ConsumeResult::SUCCESS:rocketmq::ConsumeResult::FAILURE;}
                catch(...){return rocketmq::ConsumeResult::FAILURE;}
            }).build()){}
    static rocketmq::Configuration configuration(const RocketMqConfig&c){
        static std::once_flag logging;
        std::call_once(logging,[]{auto&log=rocketmq::getLogger();log.setLevel(rocketmq::Level::Warn);log.setConsoleLevel(rocketmq::Level::Warn);log.setFileSize(4*1024*1024);log.setFileCount(3);log.init();});
        return rocketmq::Configuration::newBuilder().withEndpoints(c.endpoint).withSsl(c.tls).withRequestTimeout(std::chrono::milliseconds(2000)).withCallbackThreads(2).build();
    }
    ~Impl(){consumer.shutdown();}
};
RocketMqBroker::RocketMqBroker(RocketMqConfig config,BrokerConsumer consumer):impl_(std::make_unique<Impl>(std::move(config),std::move(consumer))){}
RocketMqBroker::~RocketMqBroker()=default;
bool RocketMqBroker::publish(const BrokerMessage &m){
    if(m.body.size()>65536)return false;
    auto builder=rocketmq::Message::newBuilder();builder.withTopic(m.topic).withKeys({m.key}).withBody(m.body);
    if(m.fifo)builder.withGroup(m.group);
    auto message=builder.build();
    auto completion=std::make_shared<std::promise<bool>>();auto result=completion->get_future();
    auto pending=impl_->pending;auto completed=std::make_shared<std::atomic<bool>>(false);
    rocketmq::SendCallback callback=[completion,pending,completed](const std::error_code&error,rocketmq::SendReceipt&&){if(completed->exchange(true))return;pending->fetch_sub(1,std::memory_order_relaxed);try{completion->set_value(!error);}catch(...){}};
    auto count=pending->load(std::memory_order_relaxed);
    do{if(count>=16)return false;}while(!pending->compare_exchange_weak(count,count+1,std::memory_order_relaxed));
    // 使用 FIFO 专用公开接口。安装脚本的明确补丁让最终错误返回调用方，
    // 不再在 SDK 内无限重入；重试、租约和死信只由持久 Outbox 统一负责。
    try{if(m.fifo)impl_->fifo.send(std::move(message),std::move(callback));else impl_->normal.send(std::move(message),std::move(callback));}
    catch(...){if(!completed->exchange(true))pending->fetch_sub(1,std::memory_order_relaxed);return false;}
    // 等待只发生在有界 Outbox 发布线程。迟到回调拥有 completion，不访问已销毁栈帧。
    // SDK 每次 RPC 2 秒、最多 3 次尝试，另给路由查询余量；仍只占专用发布线程。
    return result.wait_for(std::chrono::seconds(8))==std::future_status::ready&&result.get();
}
}
