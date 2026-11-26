#include "../FleetWire/FleetWire.h"

#include <chrono>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <utility>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    try {
        const auto command = argc >= 3 ? std::string_view(argv[2]) : "";
        if (!((argc == 3 && command == "load") || (argc == 4 && command == "status") ||
              (argc == 5 && command == "submit") || ((argc==4 || argc==5) && command=="list"))) {
            throw Error(ErrorCode::invalid, "usage: nebula_fleet_ctl 127.0.0.1:PORT (load | status ID | submit ID alpha|beta)");
        }
        grpc::ClientContext context;
        context.AddMetadata(tokenHeader, environmentToken("NEBULA_FLEET_OPERATOR_TOKEN"));
        context.set_deadline(std::chrono::system_clock::now() + 2s);
        auto stub = v1::Fleet::NewStub(localChannel(argv[1]));
        if(command=="list") {
            unsigned limit{};const std::string_view text(argv[3]); const auto parsed=std::from_chars(text.data(),text.data()+text.size(),limit);
            if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !limit || limit>16) throw Error(ErrorCode::invalid,"invalid list limit");
            v1::TaskListQuery query; query.set_limit(limit); if(argc==5) query.set_cursor(argv[4]);
            v1::TaskList result;const auto status=stub->ListTasks(&context,query,&result);
            if(!status.ok()) { std::cerr<<"rpc_status="<<status.error_code()<<'\n';return 2; }
            std::cout<<'[';bool first=true;
            for(const auto& task:result.tasks()) {
                if(!std::exchange(first,false)) std::cout<<',';
                std::cout<<"{\"taskId\":"<<std::quoted(task.command().task_id())<<",\"traceId\":"<<std::quoted(task.command().trace_id())
                    <<",\"epoch\":"<<task.command().epoch()<<",\"phase\":"<<std::quoted(name(static_cast<Phase>(task.phase())))
                    <<",\"robotId\":"<<std::quoted(task.robot_id())<<",\"outcome\":"<<std::quoted(task.outcome())
                    <<",\"connected\":"<<(task.connected()?"true":"false")<<",\"needsReconciliation\":"<<(task.needs_reconciliation()?"true":"false")<<'}';
            }
            std::cout<<"]\n";return 0;
        }
        if (command == "load") {
            v1::LoadQuery query;
            v1::LoadUpdate load;
            const auto status = stub->Load(&context, query, &load);
            if (!status.ok()) {
                std::cerr << "rpc_status=" << status.error_code() << ' ' << status.error_message() << '\n';
                return 2;
            }
            std::cout << "{\"queued\":" << load.queued() << ",\"active\":" << load.active()
                      << ",\"connected\":" << load.connected() << ",\"queueLimit\":" << load.queue_limit()
                      << ",\"watchers\":" << load.watchers() << ",\"watchRejections\":" << load.watch_rejections()
                      << "}\n";
            return 0;
        }
        v1::TaskStatus task;
        grpc::Status result;
        if (command == "status") {
            v1::TaskQuery query;
            query.set_task_id(argv[3]);
            result = stub->Status(&context, query, &task);
        } else {
            v1::TaskRequest request;
            request.set_task_id(argv[3]);
            const std::string_view station(argv[4]);
            if (station != "alpha" && station != "beta") throw Error(ErrorCode::invalid, "invalid station selection");
            request.set_station(station == "alpha" ? v1::STATION_ALPHA : v1::STATION_BETA);
            result = stub->Submit(&context, request, &task);
        }
        if (!result.ok()) {
            std::cerr << "rpc_status=" << result.error_code() << ' ' << result.error_message() << '\n';
            return 2;
        }
        std::cout << "{\"taskId\":" << std::quoted(task.command().task_id())
                  << ",\"traceId\":" << std::quoted(task.command().trace_id())
                  << ",\"epoch\":" << task.command().epoch() << ",\"phase\":\"" << name(static_cast<Phase>(task.phase()))
                  << "\",\"robotId\":" << std::quoted(task.robot_id()) << ",\"outcome\":" << std::quoted(task.outcome())
                  << ",\"connected\":" << (task.connected() ? "true" : "false")
                  << ",\"needsReconciliation\":" << (task.needs_reconciliation() ? "true" : "false") << '}' << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fleet client: " << error.what() << '\n';
        return 1;
    }
}
