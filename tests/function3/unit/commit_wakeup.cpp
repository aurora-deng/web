#include "backend/application/observability/CommitWakeClock/CommitWakeClock.h"
#include <chrono>

int main() {
    using namespace std::chrono;
    using Clock=webserver::application::CommitWakeClock::Clock;
    webserver::application::CommitWakeClock clock;
    const auto at=[](int value){return Clock::time_point{}+microseconds(value);};
    if(clock.dispatchStarted(at(0)))return 1; // Polling is not a commit sample.
    clock.committed(at(100));clock.committed(at(120));
    if(clock.dispatchStarted(at(150))!=microseconds(50))return 2; // Earliest pending notification.
    if(clock.dispatchStarted(at(180)))return 3; // Consume a coalesced wake only once.
    clock.committed(at(200));
    if(clock.dispatchStarted(at(190))!=microseconds(0))return 4;
    clock.committed(at(0));
    if(clock.dispatchStarted(at(5))!=microseconds(5))return 5; // Zero is a valid time, not "empty".
}
