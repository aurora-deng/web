#include "BusinessOperation.h"
#include "backend/application/observability/catalog/OperationCatalog.h"
#include "backend/application/observability/tracing/DetachedSpan.h"
#include <array>
#include <exception>
#include <locale>
#include <sstream>

namespace webserver::telemetry {
namespace {
constexpr auto first=static_cast<unsigned>(Series::ChatConversations);
static_assert(operationDefinitions.size()+first==static_cast<unsigned>(Series::Count));
struct Counters { std::atomic_uint64_t active{}; std::array<std::atomic_uint64_t,4> completed{}; };
std::array<Counters,operationDefinitions.size()> counters;
bool valid(Series series) noexcept {
    const auto index=static_cast<unsigned>(series);
    return index>=first&&index<static_cast<unsigned>(Series::Count);
}
}
std::string_view operationName(Series series) noexcept {
    return valid(series)?operationDefinitions[static_cast<unsigned>(series)-first].name:std::string_view{};
}
AsyncBusinessOperation::AsyncBusinessOperation() noexcept=default;
void AsyncBusinessOperation::start(Series series,bool traced) noexcept {
    if(!metricsEnabled()||!valid(series)||started_!=Clock::time_point{})return;
    series_=series;started_=Clock::now();
    counters[static_cast<unsigned>(series)-first].active.fetch_add(1,std::memory_order_relaxed);
    finished_.store(false,std::memory_order_release);
    if(traced&&tracingEnabled())try{span_=std::make_unique<DetachedSpan>(series);}catch(...){recordTraceDrop();}
}
void AsyncBusinessOperation::finish(Outcome outcome) noexcept {
    if(finished_.exchange(true,std::memory_order_acq_rel))return;
    const auto index=static_cast<unsigned>(series_)-first;
    observe(series_,elapsedUs(started_));
    counters[index].completed[static_cast<unsigned>(outcome)].fetch_add(1,std::memory_order_relaxed);
    counters[index].active.fetch_sub(1,std::memory_order_relaxed);
    if(span_)span_->finish(outcome==Outcome::Cancelled?0:outcome==Outcome::Success?1:2);
}
AsyncBusinessOperation::~AsyncBusinessOperation(){finish(Outcome::Cancelled);}
BusinessOperation::BusinessOperation(Series series,std::source_location location,bool background,std::string_view traceparent) noexcept
    :span_(operationName(series),traceparent,Span::Kind::Internal,background,location),exceptions_(std::uncaught_exceptions()) {
    measurement_.start(series);
}
BusinessOperation::~BusinessOperation(){
    if(std::uncaught_exceptions()>exceptions_)fail();
    measurement_.finish(outcome_);
}
std::string operationMetrics(){
    if(!metricsEnabled())return {};
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<"# TYPE nebula_function_calls_total counter\n# TYPE nebula_function_in_flight gauge\n";
    constexpr std::array<std::string_view,4> outcomes{"success","error","cancelled","timeout"};
    for(unsigned i=0;i<operationDefinitions.size();++i){
        const auto name=operationDefinitions[i].name;
        out<<"nebula_function_in_flight{function=\""<<name<<"\"} "<<counters[i].active.load(std::memory_order_relaxed)<<'\n';
        for(unsigned j=0;j<outcomes.size();++j)
            out<<"nebula_function_calls_total{function=\""<<name<<"\",outcome=\""<<outcomes[j]<<"\"} "
               <<counters[i].completed[j].load(std::memory_order_relaxed)<<'\n';
    }
    return out.str();
}
}
