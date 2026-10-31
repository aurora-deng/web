#include "backend/application/modules/contacts/transport/UserDirectoryHttp/UserDirectoryHttp.h"
#include "ContactsRuntime.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/modules/contacts/application/SocialService/SocialService.h"
#include "backend/application/modules/contacts/transport/ContactsHttp/ContactsHttp.h"
#include "backend/server/Runtime/ServerRuntime.h"
namespace webserver::bootstrap {
struct ContactsRuntime::Impl {
    StorageRuntime& storage;
    phase11::SocialService service;
    contacts::UserDirectoryService directory;
    std::unique_ptr<contacts::UserDirectoryHttp> directoryHttp;
    std::unique_ptr<phase11::transport::ContactsHttp>http;
    explicit Impl(StorageRuntime&s):storage(s),service(s.store(),s.store(),s.store(),s.store()),directory(s.store(),s.store()){}
};
ContactsRuntime::ContactsRuntime(StorageRuntime&s):impl_(std::make_unique<Impl>(s)){}
ContactsRuntime::~ContactsRuntime()=default;
phase11::SocialService& ContactsRuntime::service(){return impl_->service;}
void ContactsRuntime::bind(IdentityRuntime&i,ServerRuntime&n,std::function<void()>c){
    if(impl_->http)throw std::logic_error("contacts transport already bound");
    impl_->directoryHttp=std::make_unique<contacts::UserDirectoryHttp>(i.http(),impl_->storage.executor(),impl_->directory);
    impl_->http=std::make_unique<phase11::transport::ContactsHttp>(i.http(),impl_->service,impl_->storage.executor(),n.wsManager(),std::move(c));
}
void ContactsRuntime::registerRoutes(Router&r){if(!impl_->http)throw std::logic_error("contacts transport not bound");impl_->http->registerRoutes(r);impl_->directoryHttp->registerRoutes(r);}
}
