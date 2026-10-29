#include "backend/application/modules/shared/infrastructure/ProcessRunner/ProcessRunner.h"
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace webserver::function3;
int main(){try{
    auto normal=runProcess({"/usr/bin/printf","hello"},std::chrono::seconds(2));
    if(normal.status||normal.output!="hello")throw std::runtime_error("ordinary child process");
    auto output=runProcess({"/usr/bin/head","-c","65536","/dev/zero"},std::chrono::seconds(2));
    if(output.status||output.output.size()!=32768)throw std::runtime_error("worker output not bounded");
    const int inherited=open("/dev/null",O_RDONLY);if(inherited<3)throw std::runtime_error("test descriptor");
    auto fd=runProcess({"/usr/bin/readlink","/proc/self/fd/"+std::to_string(inherited)},std::chrono::seconds(2));close(inherited);
    if(!fd.status)throw std::runtime_error("child inherited a server descriptor");
    auto timeout=runProcess({"/usr/bin/sleep","20"},std::chrono::seconds(1));
    if(!timeout.timedOut||timeout.status!=137)throw std::runtime_error("child timeout was not reaped");
    std::atomic<bool>stop{};std::thread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));stop=true;});
    const auto before=std::chrono::steady_clock::now();
    auto cancelled=runProcess({"/usr/bin/sleep","20"},std::chrono::seconds(30),1024,{},&stop);cancel.join();
    if(!cancelled.cancelled||cancelled.timedOut||std::chrono::steady_clock::now()-before>std::chrono::seconds(2))throw std::runtime_error("shutdown cancellation was not prompt");
    std::cout<<"Child fd isolation, bounded output, timeout, cancellation and reap passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
