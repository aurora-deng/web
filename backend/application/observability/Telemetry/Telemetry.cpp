#include "backend/application/observability/Telemetry/Telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>
#include <openssl/rand.h>
#if defined(NEBULA_HAS_OTLP_CURL)
#include <curl/curl.h>
#endif

namespace webserver::telemetry
{
namespace
{
constexpr std::array<std::int64_t, 14> bounds{
    250, 500, 1000, 2500, 5000, 10000, 20000, 40000, 80000, 160000, 320000, 640000, 1280000, 5000000};
struct Histogram
{
    std::array<std::atomic_uint64_t, bounds.size() + 1> bins{};
    std::atomic_uint64_t sum{0};
};
std::array<Histogram, static_cast<unsigned>(Series::Count)> histograms;
thread_local std::int64_t queueWait = -1;
std::atomic_uint64_t dropped{0};
std::atomic_uint64_t exported{0};
std::atomic_uint64_t saturated{0};

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
        if (!endpoint_.empty() || logLevel() != LogLevel::Off)
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
    bool enabled() const noexcept { return worker_.joinable(); }

    void enqueue(std::string span, bool failed)
    {
        // The lock guards only the queue. Network retries run after it is released.
        std::unique_lock lock(mutex_);
        if (stop_)
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (queue_.size() >= capacity_)
        {
            saturated.fetch_add(1, std::memory_order_relaxed);
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back({std::move(span), failed});
        lock.unlock();
        ready_.notify_one();
    }

private:
    struct Entry { std::string span; bool failed; };
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
                    dropped.fetch_add(queue_.size(), std::memory_order_relaxed);
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
                    if (logLevel() == LogLevel::Info || (entry.failed && logLevel() == LogLevel::Error))
                        std::fprintf(stderr, "NEBULA_TELEMETRY %s\n", entry.span.c_str());
#if defined(NEBULA_HAS_OTLP_CURL)
                if (!endpoint_.empty())
                {
                    std::string payload = R"({"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"nebula-webserver"}}]},"scopeSpans":[{"scope":{"name":"nebula.observability","version":"1"},"spans":[)";
                    for (const auto &entry : batch)
                    {
                        payload += entry.span;
                        payload += ',';
                    }
                    payload.back() = ']';
                    payload += "}]}]}";
                    (transmit(curl, payload) ? exported : dropped).fetch_add(batch.size(), std::memory_order_relaxed);
                }
#endif
            }
            catch (...)
            {
                dropped.fetch_add(batch.size(), std::memory_order_relaxed);
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

std::int64_t elapsedUs(Clock::time_point start) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

void observe(Series series, std::int64_t microseconds) noexcept
{
    if (microseconds < 0 || !metricsEnabled())
        return;
    auto &histogram = histograms[static_cast<unsigned>(series)];
    const auto bin = std::lower_bound(bounds.begin(), bounds.end(), microseconds) - bounds.begin();
    histogram.bins[bin].fetch_add(1, std::memory_order_relaxed);
    histogram.sum.fetch_add(microseconds, std::memory_order_relaxed);
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
    text << "# TYPE " << http << " histogram\n";
    constexpr std::array<std::string_view, 7> names{
        "conversations", "history", "queue", "pool", "before_commit", "commit", "ack_ready"};
    for (unsigned i = 0; i < names.size(); ++i)
    {
        const auto name = i < 2 ? http : chat;
        if (i == 2)
            text << "# TYPE " << chat << " histogram\n";
        const std::string label = std::string(i < 2 ? "route" : "stage") + "=\"" + std::string(names[i]) + "\"";
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
    text << "# TYPE nebula_telemetry_exported_total counter\nnebula_telemetry_exported_total " << exported.load() << '\n'
         << "# TYPE nebula_telemetry_dropped_total counter\nnebula_telemetry_dropped_total " << dropped.load() << '\n'
         << "# TYPE nebula_telemetry_queue_full_total counter\nnebula_telemetry_queue_full_total " << saturated.load() << '\n';
    return text.str();
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

Span::Span(std::string_view name, std::string_view traceparent, bool server) noexcept : server_(server)
{
    if (!metricsEnabled())
        return;
    try
    {
        if (!exporter().enabled() || (!server && !current_))
            return;
        // Only a fixed set of server operations supplies names, never paths or user inputs.
        name_ = name;
        id_ = identifier(8);
        if (current_)
        {
            trace_ = current_->trace_;
            parent_ = current_->id_;
        }
        else if (validParent(traceparent))
        {
            trace_ = traceparent.substr(3, 32);
            parent_ = traceparent.substr(36, 16);
            if (std::string_view("13579bdf").find(traceparent[54]) == std::string_view::npos)
                return; // W3C sampled flag is the lowest bit of the last hexadecimal digit.
        }
        else if (logLevel() != LogLevel::Off)
            trace_ = identifier(16); // Local diagnostic logging can create its own trace.
        if (trace_.empty() || id_.empty())
            return;
        unixStart_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        steadyStart_ = Clock::now();
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
    current_ = previous_;
    try
    {
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - steadyStart_).count();
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << "{\"traceId\":\"" << trace_ << "\",\"spanId\":\"" << id_
             << "\",\"name\":\"" << name_ << "\",\"kind\":" << (server_ ? 2 : 3)
             << ",\"startTimeUnixNano\":\"" << unixStart_ << "\",\"endTimeUnixNano\":\"" << unixStart_ + duration
             << "\",\"status\":{\"code\":" << (error_ ? 2 : 1) << '}';
        if (!parent_.empty())
            json << ",\"parentSpanId\":\"" << parent_ << '"';
        json << '}';
        exporter().enqueue(json.str(), error_);
    }
    catch (...)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
}
}
