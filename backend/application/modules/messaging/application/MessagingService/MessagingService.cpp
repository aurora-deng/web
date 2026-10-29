#include "MessagingService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"
namespace webserver::function3 {
void MessagingService::administrator(Id actor)const{if(activeActor(actors_,actor).role!=Role::Administrator)throw BusinessError(Failure::Forbidden,"site administrator required");}
std::vector<QueueState>MessagingService::status(Id actor)const{administrator(actor);return operations_.status();}
std::vector<DeadEvent>MessagingService::dead(Id actor,Id before)const{administrator(actor);return operations_.dead(before);}
std::vector<DeliveryFailure>MessagingService::failures(Id actor,Id before)const{administrator(actor);return operations_.failures(before);}
void MessagingService::replay(Id actor,Id event,std::string reason,Milliseconds now){reason=trimText(std::move(reason));if(!textLength(reason,200))throw BusinessError(Failure::Invalid,"replay audit reason required");work_.transact([&]{administrator(actor);if(!operations_.retry(event))throw BusinessError(Failure::Conflict,"dead event required");events_.appendEvent({0,"admin.audit","outbox",event,static_cast<std::uint64_t>(now),{{"actorId",std::to_string(actor)},{"action","replay"},{"reason",reason}},now});});}
}
