#ifndef LOGGER_H
#define LOGGER_H

#include<string>


enum class LogLevel
{
    INFO,
    DEBUG,
    WARN,
    ERROR,
    HTTP,
    REACTOR,
    ROUTER,
    DB
};

class Logger
{
    public:
    static Logger& instance();

    void log(LogLevel level,const std::string& msg,const char *file,int line);

    private:
    Logger()=default;
    
};


#define LOG_INFO(msg)   \
    Logger::instance().log(LogLevel::INFO,msg,__FILE__,__LINE__)

#define LOG_ERROR(msg)   \
    Logger::instance().log(LogLevel::ERROR,msg,__FILE__,__LINE__)

#define LOG_WARN(msg)   \
    Logger::instance().log(LogLevel::WARN,msg,__FILE__,__LINE__)

#define LOG_DEBUG(msg)   \
    Logger::instance().log(LogLevel::DEBUG,msg,__FILE__,__LINE__)


#define LOG_HTTP(msg)   \
    Logger::instance().log(LogLevel::HTTP,msg,__FILE__,__LINE__)

#define LOG_REACTOR(msg)   \
    Logger::instance().log(LogLevel::REACTOR,msg,__FILE__,__LINE__)

#define LOG_ROUTER(msg)   \
    Logger::instance().log(LogLevel::ROUTER,msg,__FILE__,__LINE__)

#define LOG_DB(msg)   \
    Logger::instance().log(LogLevel::DB,msg,__FILE__,__LINE__)


#endif