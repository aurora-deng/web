#include "logger.h"

// ============================================================
// 文件：logger.cpp
// 职责：服务器的"异步日记本"——记录运行全过程，便于事后排查问题
//
// 【生活比喻：酒店值班日志的"双信箱"机制】
//   想象酒店前台有一本值班日记本，所有员工（业务线程）随时要记事。
//   如果大家挤在一起抢同一支笔写，会排队浪费时间；如果每个人写完都
//   立刻冲去打印机输出，又太频繁。于是采用"双信箱"方案：
//     - 前台信箱（frontBuf）：员工们把写好的便条往里塞，只需短暂加锁。
//     - 后台信箱（backBuf）：专门的文员（写线程）从里面取便条去打印。
//   信箱塞满（1024 条）或每隔 500ms，文员就把两个信箱"对调"——
//   员工立刻往新前台信箱塞，文员慢慢打印旧前台信箱的内容。
//   这样员工记事几乎不卡顿，文员打印也不打扰员工。
//
// 关键技术点（初学者重点理解）：
//   1. 双缓冲（double buffering）：前台写、后台读互不干扰，swap 后各干各的，
//      业务线程持锁时间仅一次 push_back，不会被慢速 write() 阻塞。
//   2. 条件变量超时唤醒：写线程最多睡 500ms 就主动检查，保证低频日志
//      也能及时输出，不会无限积压。
//   3. 批量 flush：后台线程一次性输出所有便条后统一 flush，比逐行 flush
//      减少 std::cout 的锁竞争和系统调用次数。
//   4. Pimpl 模式：Impl 结构体藏在 .cpp 中，头文件只暴露指针，减少编译依赖。
// ============================================================

// 单条日志：已格式化好的完整一行，后台线程直接输出即可
struct LogEntry
{
    std::string line; // 已格式化好的完整一行（含时间、级别、线程号、位置、正文）
};

/**
 * @brief Logger 的内部实现（Pimpl 惯用法）
 *
 * 【为什么用 Pimpl】把 mutex、缓冲区等细节藏在 .cpp 里，logger.h 只需前置声明
 *   struct Impl + 一个指针。这样修改缓冲区实现时不用重新编译所有 include logger.h
 *   的文件，加快编译速度；同时隐藏实现细节。
 */
struct Logger::Impl
{
    std::mutex mtx;             // 保护 frontBuf 的互斥锁（业务线程多生产，必须加锁）
    std::condition_variable cv; // 唤醒后台写线程：缓冲满或超时
 
    // 双缓冲：前台供业务线程写入，后台供写线程输出
    // 【为什么需要两个 buffer】swap 之后，业务线程写 frontBuf、写线程读 backBuf，
    //   两者操作的 vector 不同，除了 swap 瞬间外几乎无锁竞争。
    std::vector<LogEntry> frontBuf; // 业务线程写入（生产侧）
    std::vector<LogEntry> backBuf;  // 写线程输出（消费侧）
 
    size_t flushThreshold = 1024; // 前台缓冲达到此数量时唤醒写线程（水位线）
 
    Impl()
    {
        // 预留容量避免反复 realloc 导致迭代器失效和内存碎片
        // +256 是余量，防止恰好满载时 swap 后还要扩容
        frontBuf.reserve(flushThreshold + 256);
        backBuf.reserve(flushThreshold + 256);
    }
};
 
/**
 * @brief 构造函数：创建内部实现并启动后台写线程
 *
 * 【文员上岗】构造时把"文员"（写线程）派到岗位上，文员的日常工作循环：
 *   1. 等候通知（最多 500ms 超时）——信箱有便条或该下班了就醒来。
 *   2. 加锁 swap 前后台信箱——瞬间完成，员工几乎无感。
 *   3. 无锁输出后台信箱内容——慢慢打印，不打扰员工继续往新前台信箱塞便条。
 */
Logger::Logger() : impl_(new Impl)
{
    // 启动后台写线程：整个生命周期都在这个 lambda 循环里
    writer_ = std::thread([this]
                          {
        // running_ 为 false 且 backBuf 已空时才退出（确保残余日志输出完）
        while (running_.load(std::memory_order_relaxed) || !impl_->backBuf.empty())
        {
            // ---- 步骤1：等待唤醒或超时 ----
            // wait_for 第三参数是断言，返回 true 时提前醒来（有日志或该退出了）
            {
                std::unique_lock<std::mutex> lock(impl_->mtx);
                impl_->cv.wait_for(lock, std::chrono::milliseconds(500), [this]
                                   { return !impl_->frontBuf.empty() || !running_.load(std::memory_order_relaxed); });
            }
 
            // ---- 步骤2：交换前后台缓冲（持锁时间极短，仅一次 swap）----
            {
                std::lock_guard<std::mutex> lock(impl_->mtx);
                impl_->frontBuf.swap(impl_->backBuf);
            }
 
            // ---- 步骤3：输出后台缓冲（无锁，只有写线程访问 backBuf）----
            for (auto &entry : impl_->backBuf)
            {
                std::cout << entry.line;
            }
            if (!impl_->backBuf.empty())
            {
                std::cout << std::flush; // 批量 flush，而非每行 flush（减少系统调用）
                impl_->backBuf.clear();
            }
        } });
}
 
/**
 * @brief 析构函数：优雅关闭——通知写线程收尾，确保残余日志全部输出
 *
 * 【文员下班前的交接】
 *   1. 把 running_ 置 false 并唤醒写线程（notify_all）。
 *   2. join 等待写线程把 backBuf 输出完毕后退出。
 *   3. 兜底输出 frontBuf 残余（swap 之后新产生的）。
 *   4. 释放 Impl 内存。
 *   顺序很关键：必须先通知再 join，否则写线程会永远阻塞在 wait_for 上。
 */
Logger::~Logger()
{
    running_.store(false, std::memory_order_relaxed); // 标记"该下班了"
    impl_->cv.notify_all();                           // 唤醒可能在沉睡的写线程
    if (writer_.joinable())
        writer_.join(); // 等写线程把 backBuf 输出完
 
    // 输出残余日志：swap 之后业务线程可能又往 frontBuf 塞了新便条
    for (auto &entry : impl_->frontBuf)
        std::cout << entry.line;
    for (auto &entry : impl_->backBuf)
        std::cout << entry.line;
    std::cout << std::flush;
 
    delete impl_; // 释放 Pimpl 内部资源
}
 
/**
 * @brief 获取 Logger 全局单例
 *
 * 【为什么用单例】整个进程共享一本日记本即可，避免多实例各自维护写线程
 *   造成资源浪费和输出交错。C++11 起 static 局部变量初始化是线程安全的。
 *
 * @return Logger 单例引用
 */
Logger &Logger::instance()
{
    static Logger logger; // Meyers 单例：首次调用时构造，线程安全
    return logger;
}
 
/**
 * @brief 将日志级别枚举转换为固定宽度的字符串标签
 *
 * 【为什么固定宽度】所有级别标签都是 9 字符（如 "[INFO ]   "），
 *   输出时列对齐，方便肉眼快速扫描定位。
 *
 * @param level 日志级别枚举
 * @return 对应的字符串标签
 */
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
    return "[?????]   "; // 未知级别兜底
}
 
/**
 * @brief 记录一条日志（业务线程调用）
 *
 * 【员工写便条的完整流程】
 *   1. 在栈上格式化完整日志行（时间+级别+线程号+源码位置+正文），
 *      这一步不持锁，避免其他员工排队等格式化。
 *   2. 加锁把格式化好的字符串 move 进 frontBuf——持锁时间极短。
 *   3. 若 frontBuf 达到水位线（1024 条），唤醒写线程来 swap。
 *
 * @param level 日志级别
 * @param msg   日志正文
 * @param file  调用处的源文件名（由宏 __FILE__ 自动填入）
 * @param line  调用处的行号（由宏 __LINE__ 自动填入）
 */
void Logger::log(LogLevel level, const std::string &msg, const char *file, int line)
{
    // ---- 步骤1：格式化日志行（在业务线程完成，只操作栈变量，不持锁）----
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
 
    std::tm tm;
    localtime_r(&t, &tm); // 线程安全的 localtime（普通 localtime 有全局锁且非线程安全）
 
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T") << " " // 日期时间
        << levelStr(level)                     // 级别标签
        << "TID:" << std::this_thread::get_id() // 线程号（多线程排查必备）
        << " | " << file << ":" << line         // 源码位置
        << " | " << msg << "\n";                // 正文
 
    // ---- 步骤2：追加到前台缓冲区（持锁时间极短：一次 move + push_back）----
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->frontBuf.push_back(LogEntry{oss.str()}); // move 语义，无深拷贝
        shouldNotify = (impl_->frontBuf.size() >= impl_->flushThreshold);
    }
 
    // ---- 步骤3：达到水位线时唤醒写线程 ----
    // 【为什么不在锁内 notify】notify 在锁外更高效，被唤醒的线程不必等锁释放
    if (shouldNotify)
    {
        impl_->cv.notify_one();
    }
}