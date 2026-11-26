#include "backend/fleet/application/TaskWorkflow/TaskWorkflow.h"
#include "backend/systems/SqliteWorkflowJournal/SqliteWorkflowJournal.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
using namespace nebula::fleet;
using namespace std::chrono_literals;
class Driver final : public NavigationSkill {
public:
    explicit Driver(bool crash) : crash_(crash) {}
    Capabilities stations() const noexcept override { return 1; }
    bool ready() override { return true; }
    NavigationResult navigate(Station,std::stop_token,std::chrono::steady_clock::time_point) override {
        { std::ofstream file("actuations.txt",std::ios::app); file<<"navigate\n"; file.flush(); if(!file) throw std::runtime_error("effect fixture failed"); }
        if(crash_) std::_Exit(86);
        return NavigationResult::arrived;
    }
private: bool crash_;
};
int main(int argc,char* argv[]) { try {
    if(argc!=3) return 4;
    nebula::systems::SqliteWorkflowJournal journal("task.workflow.sqlite");
    Driver driver(std::string_view(argv[1])=="crash");
    TaskWorkflow task(driver,100ms,journal);
    const auto outcome=task.run({"recovery-task","1234567890abcdef1234567890abcdef",Station::alpha,std::stoll(argv[2])},{},{});
    std::cout<<name(outcome.phase)<<' '<<outcome.code<<'\n'; return 0;
} catch(const std::exception&) { std::cerr<<"identity_conflict\n"; return 3; } }
