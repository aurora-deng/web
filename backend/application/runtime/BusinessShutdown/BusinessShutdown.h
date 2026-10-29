#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
namespace webserver::application {
    /** Explicit stop order. Resources remain alive until accepted tasks stop using them. */
    class BusinessShutdown final {
        public:
        using Stop = std::function<void()>;
        void add(std::string name, Stop stop) {
            if(stopped_) throw std::logic_error("cannot register after shutdown");
            steps_.push_back({
                std::move(name),std::move(stop)
            }
            );
        }
        void stop() noexcept {
            if(stopped_)return;
            stopped_=true;
            for(auto& step:steps_) {
                try {
                    step.stop();
                }
                catch(...) {
                    ++failures_;
                }
                // Later resources still have to stop.
            }
        }
        ~BusinessShutdown(){
            stop();
        }
        std::size_t failures()const noexcept{
            return failures_;
        }
        private:
        struct Step{
            std::string name;
            Stop stop;
        };
        std::vector<Step>steps_;
        bool stopped_{
        };
        std::size_t failures_{
        };
    };
}
