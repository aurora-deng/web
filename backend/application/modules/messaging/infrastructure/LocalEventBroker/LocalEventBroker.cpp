#include "LocalEventBroker.h"
namespace webserver::function3 {
bool LocalEventBroker::publish(const BrokerMessage&message){try{return consumer_(message);}catch(...){return false;}}
}
