#include "backend/application/observability/Telemetry/Telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>
#include <openssl/rand.h>
#include "backend/application/observability/BusinessLog/BusinessLog.h"
#include "backend/application/observability/BusinessOperation/BusinessOperation.h"
#if defined(NEBULA_HAS_OTLP_CURL)
#include <curl/curl.h>
#endif

namespace webserver::telemetry
{
namespace
{
// Preserve the existing chat-resolution buckets; AI, indexing and speech may
// legitimately exceed five seconds. +Inf is retained for overflow visibility.
constexpr std::array<std::int64_t, 19> bounds{
    250, 500, 1000, 2500, 5000, 10000, 20000, 40000, 80000, 160000, 320000, 640000, 1280000, 5000000,
    10000000, 30000000, 60000000, 120000000, 300000000};
struct Histogram
{
    std::array<std::atomic_uint64_t, bounds.size() + 1> bins{};
    std::atomic_uint64_t sum{0};
};
std::array<Histogram, static_cast<unsigned>(Series::Count)> histograms;
std::array<std::atomic_uint64_t, 5> httpResponses{};
thread_local std::int64_t queueWait = -1;
std::atomic_uint64_t dropped{0};
std::atomic_uint64_t exported{0};
std::atomic_uint64_t saturated{0};
std::atomic_uint64_t logDropped{0};
std::atomic_uint64_t logWritten{0};
std::atomic_uint64_t logSaturated{0};
std::atomic_uint64_t httpSamplingSkipped{0};
std::atomic_uint64_t httpFailureRoots{0};
const double processStartedSeconds = std::chrono::duration<double>(
    std::chrono::system_clock::now().time_since_epoch()).count();

#if defined(NEBULA_HAS_OTLP_CURL)
std::string resourceAttributes()
{
    std::string json = R"([{"key":"service.name","value":{"stringValue":"nebula-webserver"}})";
    const auto append = [&](std::string_view key, const char *env) {
        const char *raw = std::getenv(env);
        const std::string_view value = raw ? raw : "";
        if (value.empty() || value.size() > 64 || value.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") != value.npos)
            return;
        json += R"(,{"key":")";
        json += key;
        json += R"(","value":{"stringValue":")";
        json += value;
        json += R"("}})";
    };
    append("service.instance.id", "WEB_OBSERVABILITY_INSTANCE_ID");
    append("deployment.environment.name", "WEB_OBSERVABILITY_ENVIRONMENT");
    return json + ']';
}
#endif

bool hex(std::string_view value)
{
    return !value.empty() && value.find_first_not_of("0123456789abcdef") == value.npos &&
           value.find_first_not_of('0') != value.npos;
}

std::string identifier(unsigned size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(size)) != 1)
        return {};
    std::string result;
    for (unsigned i = 0; i < size; ++i)
    {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 15]);
    }
    return hex(result) ? result : std::string{};
}

bool validParent(std::string_view parent)
{
    return parent.size() == 55 && parent.starts_with("00-") && parent[35] == '-' &&
           parent[52] == '-' && hex(parent.substr(3, 32)) && hex(parent.substr(36, 16)) &&
           parent.substr(53).find_first_not_of("0123456789abcdef") == parent.npos;
}

enum class LogLevel { Off, Error, Info };

LogLevel logLevel()
{
    static const auto configured = [] {
        const char *value = std::getenv("WEB_OBSERVABILITY_LOG");
        const std::string_view level = value ? value : "";
        if (level == "info")
            return LogLevel::Info;
        return level == "error" ? LogLevel::Error : LogLevel::Off;
    }();
    return configured;
}

unsigned traceInterval(const char *key) noexcept
{
    const char *configured = std::getenv(key);
    if (!configured)
        return 0;
    const std::string_view value(configured);
    unsigned count = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && count <= 1000000 ? count : 0;
}

bool sampleOnWorker(unsigned every, unsigned &remaining) noexcept
{
    // Each worker samples its first request and then one in every N requests.
    if (!every)
        return false;
    if (remaining)
    {
        --remaining;
        return false;
    }
    remaining = every - 1;
    return true;
}

unsigned queueCapacity()
{
    constexpr unsigned fallback = 1024;
    const char *raw = std::getenv("WEB_OBSERVABILITY_QUEUE_CAPACITY");
    if (!raw)
        return fallback;
    const std::string_view value(raw);
    if (value.empty() || value.size() > 4 || value.find_first_not_of("0123456789") != value.npos)
        return fallback;
    const unsigned parsed = std::atoi(raw);
    return parsed > 0 && parsed <= fallback ? parsed : fallback;
}

class Exporter final
{
public:
    Exporter()
    {
#if defined(NEBULA_HAS_OTLP_CURL)
        if (const char *raw = std::getenv("WEB_OBSERVABILITY_OTLP"))
        {
            std::string_view address(raw);
            constexpr std::string_view prefix = "http://127.0.0.1:";
            if (address.starts_with(prefix) && address.size() > prefix.size() &&
                address.size() <= prefix.size() + 5 &&
                address.substr(prefix.size()).find_first_not_of("0123456789") == address.npos &&
                std::atoi(raw + prefix.size()) > 0 && std::atoi(raw + prefix.size()) <= 65535 &&
                curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
                endpoint_ = std::string(address) + "/v1/traces";
        }
#endif
        if (!endpoint_.empty() || logLevel() != LogLevel::Off || business_log::enabled(business_log::Level::Fatal))
            worker_ = std::thread([this] { work(); });
    }

    ~Exporter()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }
    bool enabled() const noexcept { return !endpoint_.empty() || logLevel() != LogLevel::Off; }

    void enqueue(std::string span, bool failed)
    {
        append({std::move(span),failed,{}});
    }

    void diagnostic(std::string record)
    {
        append({{},false,std::move(record)});
    }

private:
    struct Entry { std::string span; bool failed; std::string log; };
    void append(Entry entry)
    {
        // The lock guards only the queue. Network retries run after it is released.
        std::unique_lock lock(mutex_);
        if (stop_)
        {
            (entry.log.empty()?dropped:logDropped).fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (queue_.size() >= capacity_)
        {
            (entry.log.empty()?saturated:logSaturated).fetch_add(1, std::memory_order_relaxed);
            (entry.log.empty()?dropped:logDropped).fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(std::move(entry));
        lock.unlock();
        ready_.notify_one();
    }

#if defined(NEBULA_HAS_OTLP_CURL)
    static size_t discard(char *, size_t size, size_t count, void *) { return size * count; }

    bool transmit(CURL *curl, const std::string &payload)
    {
        if (!curl)
            return false;
        auto *headers = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!headers)
            return false;
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        bool delivered = false;
        for (unsigned attempt = 0; attempt < 2; ++attempt)
        {
            long status = 0;
            const auto result = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            if (result == CURLE_OK && status == 200)
            {
                delivered = true;
                break;
            }
            if (result == CURLE_OK && status != 429 && status != 502 && status != 503 && status != 504)
                break;
            if (attempt == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headers);
        return delivered;
    }
#endif
    void work() noexcept
    {
#if defined(NEBULA_HAS_OTLP_CURL)
        CURL *curl = endpoint_.empty() ? nullptr : curl_easy_init();
        if (curl)
        {
            curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 400L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 150L);
            curl_easy_setopt(curl, CURLOPT_PROXY, "");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
        }
#endif
        for (;;)
        {
            std::deque<Entry> batch;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_)
                {
                    for(const auto &entry:queue_)
                        (entry.log.empty()?dropped:logDropped).fetch_add(1,std::memory_order_relaxed);
                    break;
                }
                while (!queue_.empty() && batch.size() < 32)
                {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
            }
            try
            {
                for (const auto &entry : batch)
                {
                    if (!entry.log.empty())
                        (business_log::output(entry.log)?logWritten:logDropped).fetch_add(1,std::memory_order_relaxed);
                    if (!entry.span.empty() && (logLevel() == LogLevel::Info || (entry.failed && logLevel() == LogLevel::Error)))
                        std::fprintf(stderr, "NEBULA_TELEMETRY %s\n", entry.span.c_str());
                }
#if defined(NEBULA_HAS_OTLP_CURL)
                const auto spanCount=std::count_if(batch.begin(),batch.end(),[](const Entry &entry){return !entry.span.empty();});
                if (!endpoint_.empty() && spanCount)
                {
                    static const std::string attributes = resourceAttributes();
                    std::string payload = R"({"resourceSpans":[{"resource":{"attributes":)" + attributes +
                        R"(},"scopeSpans":[{"scope":{"name":"nebula.observability","version":"1"},"spans":[)";
                    for (const auto &entry : batch)
                    {
                        if (entry.span.empty()) continue;
                        payload += entry.span;
                        payload += ',';
                    }
                    payload.back() = ']';
                    payload += "}]}]}";
                    (transmit(curl, payload) ? exported : dropped).fetch_add(spanCount, std::memory_order_relaxed);
                }
#endif
            }
            catch (...)
            {
                dropped.fetch_add(std::count_if(batch.begin(),batch.end(),[](const Entry &entry){return !entry.span.empty();}),std::memory_order_relaxed);
            }
        }
#if defined(NEBULA_HAS_OTLP_CURL)
        if (curl)
            curl_easy_cleanup(curl);
#endif
    }
    std::string endpoint_;
    const unsigned capacity_ = queueCapacity();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Entry> queue_;
    bool stop_ = false;
    std::thread worker_;
};

Exporter &exporter()
{
    static Exporter value;
    return value;
}
}

bool metricsEnabled() noexcept
{
    static const bool enabled = [] {
        const char *value = std::getenv("WEB_OBSERVABILITY");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

void submitDiagnostic(std::string record) noexcept
{
    try { exporter().diagnostic(std::move(record)); }
    catch (...) { logDropped.fetch_add(1,std::memory_order_relaxed); }
}
bool tracingEnabled() noexcept {
    try{return metricsEnabled()&&exporter().enabled();}
    catch(...){dropped.fetch_add(1,std::memory_order_relaxed);return false;}
}
void recordTraceDrop() noexcept {dropped.fetch_add(1,std::memory_order_relaxed);}
void submitSpan(std::string record,bool failed) noexcept {
    try{exporter().enqueue(std::move(record),failed);}
    catch(...){dropped.fetch_add(1,std::memory_order_relaxed);}
}

std::int64_t elapsedUs(Clock::time_point start) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

void observe(Series series, std::int64_t microseconds) noexcept
{
    if (microseconds < 0 || !metricsEnabled() || static_cast<unsigned>(series)>=static_cast<unsigned>(Series::Count))
        return;
    auto &histogram = histograms[static_cast<unsigned>(series)];
    const auto bin = std::lower_bound(bounds.begin(), bounds.end(), microseconds) - bounds.begin();
    histogram.bins[bin].fetch_add(1, std::memory_order_relaxed);
    histogram.sum.fetch_add(microseconds, std::memory_order_relaxed);
}

void recordHttpResponse(int status) noexcept
{
    if (status >= 100 && status < 600 && metricsEnabled())
        httpResponses[static_cast<unsigned>(status / 100 - 1)].fetch_add(1, std::memory_order_relaxed);
}

std::string metrics()
{
    if (!metricsEnabled())
        return {};
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(12);
    constexpr std::string_view http = "nebula_http_handler_seconds";
    constexpr std::string_view chat = "nebula_chat_stage_seconds";
    constexpr std::string_view functions = "nebula_function_seconds";
    text << "# TYPE " << http << " histogram\n";
    constexpr std::array<std::string_view, static_cast<unsigned>(Series::Count)> names{
        "conversations", "history", "queue", "pool", "before_commit", "commit", "ack_ready",
#define NEBULA_OPERATION(enumKey, key, domain, title, name) name,
#include "backend/application/observability/OperationCatalog/OperationCatalog.def"
#undef NEBULA_OPERATION
    };
    for (unsigned i = 0; i < names.size(); ++i)
    {
        const auto functionBegin = static_cast<unsigned>(Series::ChatConversations);
        const auto name = i < 2 ? http : i < functionBegin ? chat : functions;
        if (i == 2)
            text << "# TYPE " << chat << " histogram\n";
        if (i == functionBegin)
            text << "# TYPE " << functions << " histogram\n";
        const std::string label = std::string(i < 2 ? "route" : i < functionBegin ? "stage" : "function") + "=\"" + std::string(names[i]) + "\"";
        std::uint64_t count = 0;
        for (unsigned bin = 0; bin <= bounds.size(); ++bin)
        {
            count += histograms[i].bins[bin].load(std::memory_order_relaxed);
            text << name << "_bucket{" << label << ",le=\"";
            if (bin == bounds.size())
                text << "+Inf";
            else
                text << bounds[bin] / 1e6;
            text << "\"} " << count << '\n';
        }
        text << name << "_count{" << label << "} " << count << '\n';
        text << name << "_sum{" << label << "} " << histograms[i].sum.load(std::memory_order_relaxed) / 1e6 << '\n';
    }
    text << "# TYPE nebula_business_http_responses_total counter\n";
    for (unsigned i = 0; i < httpResponses.size(); ++i)
        text << "nebula_business_http_responses_total{status_class=\"" << i+1 << "xx\"} "
             << httpResponses[i].load(std::memory_order_relaxed) << '\n';
    text << "# TYPE nebula_process_start_time_seconds gauge\nnebula_process_start_time_seconds "
         << std::fixed << std::setprecision(6) << processStartedSeconds << std::defaultfloat << '\n';
    text << "# TYPE nebula_telemetry_exported_total counter\nnebula_telemetry_exported_total " << exported.load() << '\n'
         << "# TYPE nebula_telemetry_dropped_total counter\nnebula_telemetry_dropped_total " << dropped.load() << '\n'
         << "# TYPE nebula_telemetry_queue_full_total counter\nnebula_telemetry_queue_full_total " << saturated.load() << '\n'
         << "# TYPE nebula_business_log_written_total counter\nnebula_business_log_written_total " << logWritten.load() << '\n'
         << "# TYPE nebula_business_log_dropped_total counter\nnebula_business_log_dropped_total " << logDropped.load() << '\n'
         << "# TYPE nebula_business_log_queue_full_total counter\nnebula_business_log_queue_full_total " << logSaturated.load() << '\n'
         << "# TYPE nebula_http_trace_sampling_skipped_total counter\nnebula_http_trace_sampling_skipped_total "
         << httpSamplingSkipped.load(std::memory_order_relaxed) << '\n'
         << "# TYPE nebula_http_trace_failure_roots_total counter\nnebula_http_trace_failure_roots_total "
         << httpFailureRoots.load(std::memory_order_relaxed) << '\n';
    return text.str()+operationMetrics();
}

Latency::Latency(Series series) noexcept
    : series_(series), started_(metricsEnabled() ? Clock::now() : Clock::time_point{})
{
}
Latency::~Latency()
{
    if (started_ != Clock::time_point{})
        observe(series_, elapsedUs(started_));
}

QueueWait::QueueWait(std::int64_t microseconds) noexcept : previous_(queueWait) { queueWait = microseconds; }
QueueWait::~QueueWait() { queueWait = previous_; }
std::int64_t QueueWait::current() noexcept { return queueWait; }

thread_local Span *Span::current_ = nullptr;

std::pair<std::string,std::string> Span::context()
{
    return current_ ? std::pair{current_->trace_,current_->id_} : std::pair<std::string,std::string>{};
}
std::string Span::traceparent() noexcept {
    try{return current_?"00-"+current_->trace_+"-"+current_->id_+"-01":std::string{};}
    catch(...){return {};}
}

bool sampleChatSend() noexcept
{
    static const unsigned every = traceInterval("WEB_OBSERVABILITY_WS_TRACE_EVERY");
    if (!every || !metricsEnabled() || !exporter().enabled())
        return false;
    thread_local unsigned remaining = 0;
    return sampleOnWorker(every, remaining);
}

Span::Span(std::string_view name, std::string_view traceparent, Kind kind, bool sampledRoot,
           std::source_location location) noexcept : kind_(kind),location_(location)
{
    if (!metricsEnabled())
        return;
    try
    {
        if (name.empty() || (!current_ && kind != Kind::Server && !sampledRoot && !validParent(traceparent)) || !exporter().enabled())
            return;
        if (current_)
        {
            trace_ = current_->trace_;
            parent_ = current_->id_;
        }
        else if (validParent(traceparent))
        {
            if (std::string_view("13579bdf").find(traceparent[54]) == std::string_view::npos)
                return; // W3C sampled flag is the lowest bit of the last hexadecimal digit.
            trace_ = traceparent.substr(3, 32);
            parent_ = traceparent.substr(36, 16);
        }
        else
        {
            static const unsigned every = traceInterval("WEB_OBSERVABILITY_HTTP_TRACE_EVERY");
            thread_local unsigned remaining = 0;
            httpFailureOnly_ = !sampledRoot && !sampleOnWorker(every, remaining);
            if (!httpFailureOnly_)
                trace_ = identifier(16);
        }
        if (!httpFailureOnly_)
        {
            id_ = identifier(8);
            if (trace_.empty() || id_.empty())
                return;
        }
        // Only fixed operation names reach the exporter. No message or SQL is copied.
        name_ = name;
        unixStart_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        steadyStart_ = Clock::now();
        exceptionsAtStart_ = std::uncaught_exceptions();
        // Unsampled HTTP requests keep only the entry timing for a possible failure.
        // They never install a parent, so child operations don't build spans.
        if (httpFailureOnly_)
            return;
        previous_ = current_;
        current_ = this;
    }
    catch (...)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

Span::~Span()
{
    if (steadyStart_ == Clock::time_point{})
        return;
    if (!httpFailureOnly_)
        current_ = previous_;
    try
    {
        const bool unwinding = std::uncaught_exceptions() > exceptionsAtStart_;
        const bool failed = error_ || unwinding;
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - steadyStart_).count();
        if (httpFailureOnly_)
        {
            if (httpStatus_ < 500 && !unwinding)
            {
                httpSamplingSkipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            trace_ = identifier(16);
            id_ = identifier(8);
            if (trace_.empty() || id_.empty())
                return;
            httpFailureRoots.fetch_add(1, std::memory_order_relaxed);
        }
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << "{\"traceId\":\"" << trace_ << "\",\"spanId\":\"" << id_
             << "\",\"name\":\"" << name_ << "\",\"kind\":" << static_cast<int>(kind_)
             << ",\"startTimeUnixNano\":\"" << unixStart_ << "\",\"endTimeUnixNano\":\"" << unixStart_ + duration
             << "\",\"status\":{\"code\":" << (failed ? 2 : cancelled_ ? 0 : 1) << '}';
        if (!parent_.empty())
            json << ",\"parentSpanId\":\"" << parent_ << '"';
        if (httpStatus_ >= 100 && httpStatus_ <= 599)
            json << ",\"attributes\":[{\"key\":\"http.response.status_code\",\"value\":{\"intValue\":\""
                 << httpStatus_ << "\"}}]";
        json << '}';
        exporter().enqueue(json.str(), failed);
        if (kind_==Kind::Server || failed)
            business_log::write({failed ? business_log::Code::OperationFailed : business_log::Code::OperationCompleted,
                                 failed ? business_log::Level::Error : business_log::Level::Info,
                                 name_,httpStatus_,duration/1000,trace_,id_},location_);
    }
    catch (...)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
}
}
