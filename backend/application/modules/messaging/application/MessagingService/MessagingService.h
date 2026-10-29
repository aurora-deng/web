#pragma once
#include "backend/application/modules/messaging/ports/IMessagingOperations/IMessagingOperations.h"
namespace webserver::function3 {
class MessagingService final {
public:
    MessagingService(IUnitOfWork&work,IActorDirectory&actors,IMessagingOperations&operations,IEventWriter&events):work_(work),actors_(actors),operations_(operations),events_(events){}
    std::vector<QueueState>status(Id actor)const;
    std::vector<DeadEvent>dead(Id actor,Id before)const;
    std::vector<DeliveryFailure>failures(Id actor,Id before)const;
    void replay(Id actor,Id event,std::string reason,Milliseconds now);
private:void administrator(Id actor)const;IUnitOfWork&work_;IActorDirectory&actors_;IMessagingOperations&operations_;IEventWriter&events_;
};
}
