#include "BusinessLog.h"
#include "backend/application/observability/Telemetry/Telemetry.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cstdio>
#include <sstream>
#include <thread>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace webserver::business_log
{
namespace
{
constexpr std::array names{"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
constexpr std::array codes{"business.operation.completed", "business.operation.failed", "business.http.rejected",
                           "business.http.failed", "business.ws.rejected", "business.ws.failed", "runtime.started"};
std::atomic_uint64_t sequence{0};
std::string quoted(std::string_view input)
{
    std::string result = "\"";
    for (const unsigned char ch : input)
    {
        if (ch == '"' || ch == '\\') result += '\\';
        if (ch >= 32) result += static_cast<char>(ch);
    }
    return result + '"';
}
}

bool enabled(Level level) noexcept
{
    static const int threshold = [] {
        const char *raw = std::getenv("WEB_BUSINESS_LOG_LEVEL");
        if (!raw) raw = std::getenv("WEB_OBSERVABILITY_LOG");
        const std::string_view value = raw ? raw : "warn";
        constexpr std::array lower{"trace", "debug", "info", "warn", "error", "fatal"};
        if (value == "off") return 6;
        for (unsigned i = 0; i < lower.size(); ++i)
            if (value == lower[i]) return static_cast<int>(i);
        return 3;
    }();
    return static_cast<int>(level) >= threshold;
}

void write(const Record &record, std::source_location location) noexcept
{
    if (!enabled(record.level)) return;
    try
    {
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
#if defined(_WIN32)
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"recordId\":\"" << stamp << '-' << pid << '-'
             << sequence.fetch_add(1, std::memory_order_relaxed) << "\",\"timeUnixNano\":\"" << stamp
             << "\",\"level\":\"" << names[static_cast<unsigned>(record.level)]
             << "\",\"component\":\"business\",\"code\":\"" << codes[static_cast<unsigned>(record.code)]
             << "\",\"processId\":" << pid << ",\"threadId\":\""
             << std::hash<std::thread::id>{}(std::this_thread::get_id()) << '"';
        if (!record.operation.empty()) json << ",\"operation\":" << quoted(record.operation);
        if (record.status >= 100 && record.status <= 599) json << ",\"status\":" << record.status;
        if (record.durationUs >= 0) json << ",\"durationUs\":" << record.durationUs;
        if (!record.traceId.empty()) json << ",\"traceId\":" << quoted(record.traceId);
        if (!record.spanId.empty() && !record.traceId.empty()) json << ",\"spanId\":" << quoted(record.spanId);
        constexpr std::array errors{"invalid_argument", "not_found", "conflict", "unauthenticated", "forbidden",
                                    "rate_limited", "unavailable", "internal"};
        if (std::find(errors.begin(),errors.end(),record.errorCode)!=errors.end())
            json << ",\"errorCode\":" << quoted(record.errorCode);
        std::string_view file = location.file_name();
        const auto backend = file.find("backend/");
        file = backend != file.npos ? file.substr(backend) : file.substr(file.find_last_of("/\\")+1);
        json << ",\"sourceFile\":" << quoted(file) << ",\"sourceLine\":" << location.line() << '}';
        telemetry::submitDiagnostic(json.str());
    }
    catch (...) { /* Diagnostic allocation failure must not change a business result. */ }
}

bool output(std::string_view record) noexcept
{
    try
    {
        static const std::string directory = [] {
            const char *raw=std::getenv("WEB_BUSINESS_LOG_DIRECTORY");
            return raw ? std::string(raw) : std::string{};
        }();
        if (directory.empty())
            return std::fprintf(stderr,"NEBULA_LOG %.*s\n",static_cast<int>(record.size()),record.data())>=0;
        const std::filesystem::path root(directory);
        if (!root.is_absolute()) return false;
        std::filesystem::create_directories(root);
        const auto now=std::time(nullptr);
        const auto date=[](std::time_t value) {
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm,&value);
#else
            gmtime_r(&value,&tm);
#endif
            char text[11]{};std::strftime(text,sizeof(text),"%Y-%m-%d",&tm);return std::string(text);
        };
        static auto lastPrune=std::chrono::steady_clock::time_point{};
        if (std::chrono::steady_clock::now()-lastPrune>=std::chrono::minutes(1))
        {
            const auto cutoff=date(now-6*86400);
            for (const auto &entry:std::filesystem::directory_iterator(root))
            {
                const auto name=entry.path().filename().string();
                if (!entry.is_symlink() && entry.is_regular_file() && name.size()==21 && name.starts_with("nebula-") &&
                    name.ends_with(".log") && name.substr(7,10)<cutoff)
                    std::filesystem::remove(entry.path());
            }
            lastPrune=std::chrono::steady_clock::now();
        }
        const auto path=root/("nebula-"+date(now)+".log");
        const auto size=std::filesystem::exists(path)?std::filesystem::file_size(path):0;
        if (size+record.size()+12>8*1024*1024) return false;
        std::ofstream file(path,std::ios::app|std::ios::binary);
        std::filesystem::permissions(path,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);
        file << "NEBULA_LOG " << record << '\n';
        file.flush();
        return static_cast<bool>(file);
    }
    catch (...) { return false; }
}
}
