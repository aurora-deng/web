#include "logger.h"

// ============================================================
// 异步日志实现：双缓冲 + 后台写线程
//
// 核心思路：
//   业务线程调用 log() 时，只做格式化 + 追加到前台缓冲区（持锁时间极短）
//   后台线程每 500ms 或前台缓冲满时，交换前后台缓冲，输出后台缓冲内容
//   这样业务线程永远不会被 write() 系统调用阻塞
// ============================================================
 
// 单条日志
struct LogEntry
{
    std::string line; // 已格式化好的完整一行
};
 
struct Logger::Impl
{
    std::mutex mtx;
    std::condition_variable cv;
 
    // 双缓冲：前台供业务线程写入，后台供写线程输出
    std::vector<LogEntry> frontBuf; // 业务线程写入
    std::vector<LogEntry> backBuf;  // 写线程输出
 
    size_t flushThreshold = 1024; // 前台缓冲达到此数量时唤醒写线程
 
    Impl()
    {
        frontBuf.reserve(flushThreshold + 256);
        backBuf.reserve(flushThreshold + 256);
    }
};
 
Logger::Logger() : impl_(new Impl)
{
    // 启动后台写线程
    writer_ = std::thread([this]
                          {
        while (running_.load(std::memory_order_relaxed) || !impl_->backBuf.empty())
        {
            // 等待：要么被唤醒，要么超时 500ms
            {
                std::unique_lock<std::mutex> lock(impl_->mtx);
                impl_->cv.wait_for(lock, std::chrono::milliseconds(500), [this]
                                   { return !impl_->frontBuf.empty() || !running_.load(std::memory_order_relaxed); });
            }
 
            // 交换前后台缓冲
            {
                std::lock_guard<std::mutex> lock(impl_->mtx);
                impl_->frontBuf.swap(impl_->backBuf);
            }
 
            // 输出后台缓冲（无锁，只有写线程访问 backBuf）
            for (auto &entry : impl_->backBuf)
            {
                std::cout << entry.line;
            }
            if (!impl_->backBuf.empty())
            {
                std::cout << std::flush; // 批量 flush，而非每行 flush
                impl_->backBuf.clear();
            }
        } });
}
 
Logger::~Logger()
{
    running_.store(false, std::memory_order_relaxed);
    impl_->cv.notify_all();
    if (writer_.joinable())
        writer_.join();
 
    // 输出残余日志
    for (auto &entry : impl_->frontBuf)
        std::cout << entry.line;
    for (auto &entry : impl_->backBuf)
        std::cout << entry.line;
    std::cout << std::flush;
 
    delete impl_;
}
 
Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}
 
static const char *levelStr(LogLevel level)
{
    switch (level)
    {
    case LogLevel::DEBUG:
        return "[DEBUG]   ";
    case LogLevel::INFO:
        return "[INFO ]   ";
    case LogLevel::WARN:
        return "[WARN ]   ";
    case LogLevel::ERROR:
        return "[ERROR]   ";
    case LogLevel::HTTP:
        return "[HTTP ]   ";
    case LogLevel::REACTOR:
        return "[REACTOR] ";
    case LogLevel::ROUTER:
        return "[ROUTER]  ";
    case LogLevel::DB:
        return "[DB    ]  ";
    }
    return "[?????]   ";
}
 
void Logger::log(LogLevel level, const std::string &msg, const char *file, int line)
{
    // 1. 格式化日志行（在业务线程中完成，但只操作栈变量，不持锁）
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
 
    std::tm tm;
    localtime_r(&t, &tm);
 
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T") << " "
        << levelStr(level)
        << "TID:" << std::this_thread::get_id()
        << " | " << file << ":" << line
        << " | " << msg << "\n";
 
    // 2. 追加到前台缓冲区（持锁时间极短：一次 move + push_back）
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->frontBuf.push_back(LogEntry{oss.str()});
        shouldNotify = (impl_->frontBuf.size() >= impl_->flushThreshold);
    }
 
    // 3. 达到水位线时唤醒写线程
    if (shouldNotify)
    {
        impl_->cv.notify_one();
    }
}