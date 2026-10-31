#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"

#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/modules/accounts/application/AuthService/AuthService.h"
#include "backend/application/security/SodiumCredentialCodec/SodiumCredentialCodec.h"
#include "backend/application/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
namespace webserver::bootstrap {
namespace {
auto lifetime(){return std::chrono::seconds(size("WEB_PHASE11_SESSION_SECONDS",86400,60,31536000));}
auto options(bool production){phase11::transport::Phase11AuthHttpConfig c;c.secureCookie=production||flag("WEB_PHASE11_SECURE_COOKIE");c.sessionLifetime=lifetime();if(const char*h=std::getenv("WEB_PHASE11_TRUSTED_CLIENT_IP_HEADER"))c.trustedClientIpHeader=h;return c;}
}
struct IdentityRuntime::Impl {
    phase11::SodiumCredentialCodec credentials;
    phase11::InMemoryLoginRateLimiter limiter;
    phase11::AuthService service;
    phase11::transport::Phase11AuthHttp http;
    Impl(StorageRuntime&s,bool p):credentials(privateHexKey(std::getenv("WEB_PHASE11_TOKEN_KEY_FILE"))),
      service(s.store(),s.store(),credentials,limiter,lifetime()),http(service,s.executor(),options(p)){}
};
IdentityRuntime::IdentityRuntime(StorageRuntime&s,bool p):impl_(std::make_unique<Impl>(s,p)){}
IdentityRuntime::~IdentityRuntime()=default;
phase11::transport::Phase11AuthHttp& IdentityRuntime::http(){return impl_->http;}
}
