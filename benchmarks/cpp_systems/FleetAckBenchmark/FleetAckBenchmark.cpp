#include "backend/fleet/transport/grpc/FleetWire/FleetWire.h"
#include "backend/workflow/WorkflowGraph/WorkflowGraph.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

namespace {
using Clock = std::chrono::steady_clock;

unsigned bounded(const char* text, unsigned min, unsigned max) {
    const std::string_view input(text);
    unsigned value{};
    const auto end = input.data() + input.size();
    const auto parsed = std::from_chars(input.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < min || value > max) {
        throw std::invalid_argument("invalid benchmark workload");
    }
    return value;
}

class Benchmark {
public:
    Benchmark(std::string_view address, std::string token)
        : stub_(v1::Fleet::NewStub(localChannel(address))), token_(std::move(token)),
          prefix_(nebula::workflow::newTraceId().substr(0, 12)) {}

    double sample(unsigned id) {
        const auto start = Clock::now();
        const auto taskId = "bench." + prefix_ + "." + std::to_string(id);
        v1::TaskRequest request;
        request.set_task_id(taskId);
        request.set_station(id % 2 ? v1::STATION_ALPHA : v1::STATION_BETA);
        v1::TaskStatus reply;
        {
            grpc::ClientContext context;
            prepare(context);
            if (!stub_->Submit(&context, request, &reply).ok()) return -1;
        }
        v1::TaskQuery query;
        query.set_task_id(taskId);
        while (Clock::now() - start < 1500ms) {
            const auto phase = static_cast<Phase>(reply.phase());
            if (phase == Phase::accepted || phase == Phase::executing || phase == Phase::succeeded) {
                return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            }
            if (phase != Phase::queued && phase != Phase::leased) break;
            std::this_thread::sleep_for(2ms);
            grpc::ClientContext context;
            prepare(context);
            if (!stub_->Status(&context, query, &reply).ok()) break;
        }
        return -1;
    }

private:
    void prepare(grpc::ClientContext& context) const {
        context.set_deadline(std::chrono::system_clock::now() + 500ms);
        context.AddMetadata(tokenHeader, token_);
    }

    std::unique_ptr<v1::Fleet::Stub> stub_;
    std::string token_;
    std::string prefix_;
};

double percentile(const std::vector<double>& samples, unsigned p) {
    return samples.empty() ? 0 : samples[(samples.size() * p + 99) / 100 - 1];
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 5) throw std::invalid_argument("usage: nebula_fleet_ack_bench ENDPOINT COUNT ROUNDS PERIOD_MS");
        const auto count = bounded(argv[2], 1, 5000);
        const auto rounds = bounded(argv[3], 1, 5);
        const auto period = std::chrono::milliseconds{bounded(argv[4], 1, 1000)};
        Benchmark bench(argv[1], environmentToken("NEBULA_FLEET_OPERATOR_TOKEN"));
        for (unsigned i = 0; i < 12; ++i) {
            if (bench.sample(i) < 0) throw std::runtime_error("fleet benchmark warmup failed");
            std::this_thread::sleep_for(period);
        }
        std::cout << "round,attempted,accepted,errors,p50_ms,p95_ms,p99_ms,elapsed_s" << std::endl;
        unsigned errors = 0;
        for (unsigned round = 1; round <= rounds; ++round) {
            const auto start = Clock::now();
            std::vector<double> samples;
            samples.reserve(count);
            for (unsigned i = 0; i < count; ++i) {
                const auto due = Clock::now() + period;
                const auto value = bench.sample(round * 5000 + i + 12);
                if (value >= 0) samples.push_back(value);
                std::this_thread::sleep_until(due);
            }
            std::ranges::sort(samples);
            const auto failed = count - samples.size();
            errors += static_cast<unsigned>(failed);
            std::cout << round << ',' << count << ',' << samples.size() << ',' << failed << ','
                      << percentile(samples, 50) << ',' << percentile(samples, 95) << ','
                      << percentile(samples, 99) << ','
                      << std::chrono::duration<double>(Clock::now() - start).count() << std::endl;
        }
        return errors ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "fleet benchmark: " << error.what() << '\n';
        return 1;
    }
}
