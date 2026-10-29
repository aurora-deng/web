#include "backend/application/runtime/BusinessShutdown/BusinessShutdown.h"
#include <vector>
int main(){
    std::vector<int> order;
    webserver::application::BusinessShutdown shutdown;
    shutdown.add("producer",[&]{order.push_back(1);});
    shutdown.add("failed task",[&]{order.push_back(2);throw 1;});
    shutdown.add("executor",[&]{order.push_back(3);});
    shutdown.stop();shutdown.stop();
    if(order!=std::vector<int>{1,2,3}||shutdown.failures()!=1)return 1;
    try{shutdown.add("late",[]{});return 2;}catch(const std::logic_error&){}
}
