#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
namespace webserver::config {
    /** 启动配置的唯一校验入口；错误只输出变量名，绝不回显凭据值。 */
    class RuntimeConfig final {
        public:
        using Lookup=std::function<std::string(const char*)>;
        static std::string environment(const char*name);
        static bool flag(const char*name);
        static std::size_t size(const char*name,std::size_t fallback,std::size_t low,std::size_t high);
        static std::vector<std::string> list(std::string value);
        static RuntimeConfig load(Lookup lookup=environment);
        bool production{
        };
        std::size_t port{
            8080
        }
        ,tlsPort{
            8443
        };
        std::string authSecret,tlsCert,tlsKey,database,tokenKeyFile;
        std::vector<std::string> allowedOrigins;
    };
}
