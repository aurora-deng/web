#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct QueueState{std::string state;std::uint64_t count{};Id first{};};
struct DeadEvent{Id id{};std::string type,aggregate,error;Id aggregateId{};std::uint64_t attempts{};};
struct DeliveryFailure{Id id{},event{};std::string topic,key,error;std::uint64_t attempts{};};
class IMessagingOperations {
public:virtual ~IMessagingOperations()=default;virtual std::vector<QueueState>status()const=0;virtual std::vector<DeadEvent>dead(Id before)const=0;virtual std::vector<DeliveryFailure>failures(Id before)const=0;virtual bool retry(Id event)=0;
};
}
