#pragma once
#include "backend/server/Route/Router.h"
#include "backend/application/observability/Telemetry/Telemetry.h"
#include "backend/application/observability/BusinessLog/BusinessLog.h"
#include <cstdlib>
#include <optional>

namespace webserver::phase11::transport {
// A controlled reverse proxy can correlate its access log with a sampled
// application span. The value is resolved by the server's tracing policy.
inline void finishBusinessHttpSpan(RequestContext &context, telemetry::Span &span) noexcept
{
    if (!context.response)
        return;
    span.httpStatus(context.response->status);
    static const bool exposeToGateway = [] {
        const char *value = std::getenv("WEB_OBSERVABILITY_GATEWAY_TRACE_HEADER");
        return value && std::string_view(value) == "1";
    }();
    if (exposeToGateway && !span.traceId().empty())
    {
        try { context.response->setHeader("X-Nebula-Trace-Id", std::string(span.traceId())); }
        catch (...) { /* Response delivery must survive a telemetry allocation failure. */ }
    }
}

// Only the registered route template may name a span. Never use request.path,
// query strings, resource IDs, JSON, credentials, exception text or header values.
class BusinessHttpObservation final {
public:
    explicit BusinessHttpObservation(RequestContext &context,bool logRejection=false) noexcept
        :context_(context),logRejection_(logRejection) {
        try {
            if(!telemetry::metricsEnabled()||!context.route||!context.route->path.starts_with("/api/"))return;
            const auto &method=context.route->method;const auto &path=context.route->path;
            // Existing chat entries own these spans and their original duration metrics.
            if((method=="GET"&&path=="/api/conversations")||
               ((method=="GET"||method=="POST")&&path=="/api/conversations/:id/messages"))return;
            operation_=method+" "+path;
            const auto parent=context.request.headers.find("traceparent");
            span_.emplace(operation_,parent==context.request.headers.end()?std::string_view{}:parent->second,
                          telemetry::Span::Kind::Server);
        }catch(...){/* Telemetry never fails an accepted request. */}
    }
    ~BusinessHttpObservation(){
        if(!context_.response)return;
        const int status=context_.response->status;
        telemetry::recordHttpResponse(status);
        if (span_) finishBusinessHttpSpan(context_, *span_);
        if(logRejection_&&status>=400){
            const auto [trace,span]=telemetry::Span::context();
            business_log::write({status>=500?business_log::Code::HttpFailed:business_log::Code::HttpRejected,
                status>=500?business_log::Level::Error:business_log::Level::Warn,operation_,status,-1,trace,span,
                status>=500?"business_failed":"business_rejected"});
        }
    }
    BusinessHttpObservation(const BusinessHttpObservation &)=delete;
    BusinessHttpObservation &operator=(const BusinessHttpObservation &)=delete;
private:
    RequestContext &context_;
    bool logRejection_;
    std::string operation_;
    std::optional<telemetry::Span> span_;
};
}
