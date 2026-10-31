#include "backend/application/bootstrap/FileRuntime/FileRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"
#include "backend/application/bootstrap/ChatRuntime/ChatRuntime.h"

#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/storage/files/LocalFileStorage/LocalFileStorage.h"
#include "backend/application/modules/chat/application/ChatService/ChatService.h"
#include "backend/application/modules/files/application/FileService/FileService.h"
#include "backend/application/transport/http/Phase11FileHttp/Phase11FileHttp.h"
namespace webserver::bootstrap {
struct FileRuntime::Impl {
    phase11::LocalFileStorage storage;
    phase11::FileService service;
    phase11::BoundedExecutor executor;
    std::unique_ptr<phase11::transport::Phase11FileHttp> http;
    explicit Impl(StorageRuntime&s):storage(std::getenv("WEB_PHASE11_FILE_ROOT")?std::getenv("WEB_PHASE11_FILE_ROOT"):"phase11-files"),
      service(s.store(),s.store(),s.store(),storage),executor(size("WEB_PHASE11_FILE_WORKERS",2,1,16),size("WEB_PHASE11_FILE_QUEUE",64,1,4096)){}
};
FileRuntime::FileRuntime(StorageRuntime&s):impl_(std::make_unique<Impl>(s)){}
FileRuntime::~FileRuntime(){drain();}
phase11::FileService& FileRuntime::service(){return impl_->service;}
phase11::BoundedExecutor& FileRuntime::executor(){return impl_->executor;}
void FileRuntime::bind(IdentityRuntime&i,ChatRuntime&c,std::function<void()>notify){if(impl_->http)throw std::logic_error("file transport already bound");impl_->http=std::make_unique<phase11::transport::Phase11FileHttp>(i.http(),impl_->service,c.chat(),impl_->executor,std::move(notify));}
void FileRuntime::registerRoutes(Router&r){if(!impl_->http)throw std::logic_error("file transport not bound");impl_->http->registerRoutes(r);}
void FileRuntime::drain()noexcept{impl_->executor.shutdown();}
}
