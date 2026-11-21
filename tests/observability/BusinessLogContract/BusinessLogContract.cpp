#include "backend/application/observability/BusinessLog/BusinessLog.h"
#include "backend/application/observability/Telemetry/Telemetry.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

int main()
{
    using namespace webserver::business_log;
    const auto root=std::filesystem::temp_directory_path()/
        ("nebula-log-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        std::filesystem::create_directory(root);
#if defined(_WIN32)
        _putenv_s("WEB_BUSINESS_LOG_DIRECTORY",root.string().c_str());
        _putenv_s("WEB_BUSINESS_LOG_LEVEL","warn");
        _putenv_s("WEB_OBSERVABILITY","1");
#else
        setenv("WEB_BUSINESS_LOG_DIRECTORY",root.c_str(),1);
        setenv("WEB_BUSINESS_LOG_LEVEL","warn",1);
        setenv("WEB_OBSERVABILITY","1",1);
#endif
        std::ofstream(root/"nebula-1900-01-01.log") << "expired";
        std::ofstream(root/"unrelated.keep") << "preserved";
        const std::string trace(32,'a'),span(16,'b');
        write({Code::HttpFailed,Level::Error,"business HTTP",503,1250,trace,span,"unavailable"});
        std::filesystem::path current;
        std::string line;
        for (unsigned attempt=0;attempt<100;++attempt)
        {
            for (const auto &entry:std::filesystem::directory_iterator(root))
                if (entry.path().filename().string().starts_with("nebula-") && entry.path().filename()!="nebula-1900-01-01.log")
                    current=entry.path();
            if (!current.empty()) { std::ifstream input(current);std::getline(input,line); }
            if (!line.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (line.find("NEBULA_LOG {")!=0 || line.find("\"durationUs\":1250")==line.npos ||
            line.find("\"status\":503")==line.npos || line.find(trace)==line.npos)
            throw std::runtime_error("structured diagnostic was not written");
        if (std::filesystem::exists(root/"nebula-1900-01-01.log") || !std::filesystem::exists(root/"unrelated.keep"))
            throw std::runtime_error("owned-file retention failed");
        { std::ofstream full(current,std::ios::app|std::ios::binary); full.seekp(0,std::ios::end);
          std::string padding(1024*1024,' ');for(unsigned i=0;i<8;++i)full.write(padding.data(),padding.size()); }
        if (output("{}")) throw std::runtime_error("log file quota was not enforced");
        write({Code::HttpFailed,Level::Error,"business HTTP",503});
        std::string measurements;
        for(unsigned attempt=0;attempt<100;++attempt)
        {
            measurements=webserver::telemetry::metrics();
            if(measurements.find("nebula_business_log_dropped_total 1\n")!=measurements.npos)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(measurements.find("nebula_business_log_dropped_total 1\n")==measurements.npos ||
           measurements.find("nebula_telemetry_dropped_total 0\n")==measurements.npos)
            throw std::runtime_error("log quota loss was mixed with trace export loss");
        std::filesystem::remove(current);
        std::filesystem::remove(root/"unrelated.keep");
        std::filesystem::remove(root);
        std::cout << "PASS async structured output, trace context, seven-day rotation and file quota\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n';return 1; }
}
