#include "tests/phase11/smoke/OnnxBenchmark.h"

#include <cstdlib>
#include <iostream>
#include <thread>

using namespace webserver::phase11;
#define CHECK(x) do { if (!(x)) { std::cerr << "benchmark contract failed at " << __LINE__ << '\n'; std::abort(); } } while (false)

class Fake final : public IModelProvider
{
public:
    enum class Mode { Success, Timeout, Oversized, Duplicate, LateToken };
    explicit Fake(Mode mode) : mode_(mode) {}
    ~Fake() override { if (worker_.joinable()) worker_.join(); }
    std::string name() const override { return "benchmark-fake"; }
    bool ready() const noexcept override { return true; }
    GenerationHandle generate(const GenerationRequest &request, TokenSink sink,
                               CompletionSink done, CancellationToken cancellation) override
    {
        worker_ = std::thread([=, this] {
            if (mode_ == Mode::Timeout) {
                while (!cancellation.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                done({false, true, {}, {}, std::nullopt});
                return;
            }
            if (mode_ == Mode::Oversized) {
                sink(std::string(65537, 'x'));
                CHECK(cancellation.cancelled());
                done({false, true, {}, {}, std::nullopt});
                return;
            }
            sink("hel"); sink("lo");
            GenerationCompletion c;
            c.success = true;
            c.usage = TokenizerUsage{3, 5, 4, 1000, 100, 10, 0, true};
            done(c);
            if (mode_ == Mode::Duplicate) done({false, false, "duplicate", {}, std::nullopt});
            if (mode_ == Mode::LateToken) sink("must-not-append");
        });
        return {request.requestId, cancellation};
    }
private:
    Mode mode_;
    std::thread worker_;
};

int main()
{
    using namespace benchmark;
    const auto test = parseCase(R"({"id":"sample-1","category":"classification","system":"brief","prompt":"hello","maxTokens":"32"})");
    CHECK(test.maxTokens == 32);
    for (const auto raw : {"-1", "+1", "0", "513", "1x", " 2", "184467440737095516160"}) {
        bool rejected = false;
        try { (void)boundedNumber(raw, 512); } catch (const std::exception &) { rejected = true; }
        CHECK(rejected);
    }
    for (const auto raw : {"{}", "[]", R"({"id":"x","id":"y"})", R"({"id":"x","category":"a","system":"b","prompt":"c","maxTokens":"1","extra":"no"})"}) {
        bool rejected = false;
        try { (void)parseCase(raw); } catch (const std::exception &) { rejected = true; }
        CHECK(rejected);
    }
    for (const auto mode : {Fake::Mode::Success, Fake::Mode::Duplicate, Fake::Mode::LateToken}) {
        Fake fake(mode);
        const auto m = generate(fake, {}, std::chrono::seconds(2));
        CHECK(m.completion.success && m.text == "hello" && m.textChunks == 2);
        CHECK(m.clientFirstTokenMicroseconds && !m.timedOut);
        const auto output = json(test, 1, 1, m);
        CHECK(output.find("\"outputTokens\":5") != output.npos);
        CHECK(output.find("\"textChunks\":2") != output.npos);
    }
    {
        Fake fake(Fake::Mode::Timeout);
        const auto m = generate(fake, {}, std::chrono::milliseconds(5), std::chrono::seconds(2));
        CHECK(m.timedOut && m.completion.cancelled && !m.clientFirstTokenMicroseconds);
    }
    {
        Fake fake(Fake::Mode::Oversized);
        const auto m = generate(fake, {}, std::chrono::seconds(2));
        CHECK(m.outputLimitExceeded && m.text.empty());
        CHECK(json(test, 1, 1, m).find("\"validUsage\":false") != std::string::npos);
    }
    std::cout << "ONNX_BENCHMARK_CONTRACT_OK\n";
}
