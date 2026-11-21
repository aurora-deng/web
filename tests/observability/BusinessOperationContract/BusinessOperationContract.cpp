#include "backend/application/observability/BusinessOperation/BusinessOperation.h"
#include "backend/application/transport/http/BusinessHttpObservation/BusinessHttpObservation.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void setting(const char *name,const char *value){
#if defined(_WIN32)
    _putenv_s(name,value);
#else
    setenv(name,value,1);
#endif
}
void require(bool value,const char *why){if(!value)throw std::runtime_error(why);}
std::uint64_t counter(std::string_view prefix){
    const auto text=webserver::telemetry::metrics();const auto start=text.find(prefix);
    require(start!=text.npos,"expected fixed metric missing");
    return std::stoull(text.substr(start+prefix.size()));
}
}
int main(int argc,char **){
    using namespace webserver::telemetry;
    setting("WEB_OBSERVABILITY",argc>1?"0":"1");
    setting("WEB_OBSERVABILITY_LOG","error");setting("WEB_BUSINESS_LOG_LEVEL","off");
    try{
        if(argc>1){
            {BusinessOperation operation(Series::TreePublish);}
            AsyncBusinessOperation operation;operation.start(Series::ModelGeneration);operation.finish(Outcome::Error);
            require(metrics().empty(),"disabled instrumentation exported data");
            std::cout<<"PASS disabled instrumentation\n";return 0;
        }
        {
            BusinessOperation operation(Series::PrivateDispatch,std::source_location::current(),true);
            operation.cancel();
        }
        require(counter("nebula_function_calls_total{function=\"PrivateGenerationService::dispatch\",outcome=\"cancelled\"} ")==1,"cancelled preparation counted as success");
        {
            BusinessOperation operation(Series::TreeList);
            require(counter("nebula_function_in_flight{function=\"TreeService::list\"} ")==1,"in-flight not visible");
        }
        require(counter("nebula_function_in_flight{function=\"TreeService::list\"} ")==0,"in-flight leaked");
        require(counter("nebula_function_calls_total{function=\"TreeService::list\",outcome=\"success\"} ")==1,"success missing");
        try{BusinessOperation operation(Series::TreePublish);throw std::runtime_error("PRIVATE_CANARY_NOT_TELEMETRY");}catch(const std::exception&){}
        require(counter("nebula_function_calls_total{function=\"TreeService::publish\",outcome=\"error\"} ")==1,"unwind not failed");
        {BusinessOperation operation(Series::SpeechRender);operation.fail();}
        require(counter("nebula_function_calls_total{function=\"SpeechService::runOne\",outcome=\"error\"} ")==1,"handled failure lost");
        {
            AsyncBusinessOperation operation;operation.start(Series::ModelGeneration);
            std::vector<std::thread> racers;
            for(unsigned i=0;i<8;++i)racers.emplace_back([&]{operation.finish(Outcome::Success);});
            for(auto &thread:racers)thread.join();
        }
        for(auto outcome:{Outcome::Error,Outcome::Cancelled,Outcome::Timeout}){
            AsyncBusinessOperation operation;operation.start(Series::ModelGeneration);operation.finish(outcome);operation.finish(Outcome::Success);
        }
        for(const auto *outcome:{"success","error","cancelled","timeout"})
            require(counter(std::string("nebula_function_calls_total{function=\"ResilientModelProvider::generation\",outcome=\"")+outcome+"\"} ")==1,"async completion duplicated or lost");
        require(counter("nebula_function_in_flight{function=\"ResilientModelProvider::generation\"} ")==0,"async in-flight leaked");
        HttpResponse response;response.status=401;
        RequestContext request;request.response=&response;
        RouteEntry route;route.method="GET";route.path="/api/tree/posts/:post";request.route=&route;
        request.request.path="/api/tree/posts/PRIVATE_CANARY_NOT_TELEMETRY";
        request.request.headers["traceparent"]="00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01";
        {
            webserver::phase11::transport::BusinessHttpObservation observation(request,true);
            require(Span::context().first==std::string(32,'a'),"normalized HTTP root did not propagate context");
            {BusinessOperation operation(Series::TreeGet);require(Span::context().first==std::string(32,'a'),"business child lost parent");}
            const auto parent=Span::traceparent();
            std::atomic_bool linked{};
            std::thread worker([&]{
                BusinessOperation dispatch(Series::PrivateDispatch,std::source_location::current(),false,parent);
                linked.store(Span::context().first==std::string(32,'a'));
                AsyncBusinessOperation asynchronous;asynchronous.start(Series::ModelGeneration,true);
                std::thread completion([&]{asynchronous.finish(Outcome::Success);});completion.join();
                linked.store(linked.load()&&Span::context().first==std::string(32,'a'));
            });worker.join();require(linked.load(),"cross-worker parent or detached completion changed TLS");
        }
        require(counter("nebula_business_http_responses_total{status_class=\"4xx\"} ")==1,"HTTP response counted more than once");
        require(Span::context().first.empty(),"thread-local context leaked");
        require(metrics().find("PRIVATE_CANARY_NOT_TELEMETRY")==std::string::npos,"private data entered metrics");
        observe(Series::ModelGeneration,7'000'000); // A measured-duration fixture, no seven-second sleep.
        const std::string modelBucket="nebula_function_seconds_bucket{function=\"ResilientModelProvider::generation\",le=\"";
        require(counter(modelBucket+"10\"} ")==counter(modelBucket+"5\"} ")+1,"long task fell straight into +Inf");
        require(counter(modelBucket+"300\"} ")==counter(modelBucket+"+Inf\"} "),"long task upper buckets missing");
        std::cout<<"PASS sync/async outcomes, race dedup, gauge drain, disabled mode and template-only HTTP context\n";
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
